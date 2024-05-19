#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <liburing.h>
#include <omp.h>
#include <unistd.h>

// Global atomic counter for unique file naming

template <typename T> class GPUOperations {
public:
  void perform(T shared_ptr, int size, int num_teams, int num_threads_per_team,
               std::atomic<int> &file_counter) {
// Use OpenMP to allocate memory and distribute the workload across multiple GPU
// threads
#pragma omp target teams distribute parallel for collapse(2)                   \
    num_teams(num_teams) thread_limit(num_threads_per_team)                    \
    map(tofrom : shared_ptr[ : size])
    for (int team = 0; team < num_teams; ++team) {
      for (int thread = 0; thread < num_threads_per_team; ++thread) {
        int index = team * num_threads_per_team + thread;
        if (index < size) {
          shared_ptr[index] = 1; // Each thread sets its designated element to 1
        }
      }
    }
    
    call_io_function(shared_ptr, size, file_counter);
  }

  void increment(T shared_ptr, int size) {
// Use OpenMP to increment the data on the GPU
#pragma omp target teams distribute parallel for map(                          \
        tofrom : shared_ptr[ : size])
    for (int i = 0; i < size; ++i) {
      shared_ptr[i] += 1; // Increment each element by 1
    }
  }

#pragma omp declare target
  void call_io_function(T shared_ptr, int size, std::atomic<int> &file_counter);
};

template <typename T> class IOOperations {
public:
  void process(T shared_ptr, int size, std::atomic<int> &file_counter);
  void readAndModify(T shared_ptr, int size, const char *file_path);
};

template <typename T>
void IOOperations<T>::process(T shared_ptr, int size,
                              std::atomic<int> &file_counter) {
  int file_number = file_counter.fetch_add(1);
  pid_t pid = getpid();
  char filename[256];
  snprintf(filename, sizeof(filename), "output_%d_%d.bin", pid, file_number);

  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("Failed to open file");
    return;
  }

  struct io_uring ring;
  if (io_uring_queue_init(8, &ring, 0) != 0) {
    fprintf(stderr, "io_uring setup failed\n");
    close(fd);
    return;
  }

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_write(sqe, fd, shared_ptr, size * sizeof(int), 0);
  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;
  io_uring_wait_cqe(&ring, &cqe);
  if (cqe->res < 0) {
    fprintf(stderr, "io_uring write failed: %s\n", strerror(-cqe->res));
  } else {
    std::cout << "Write to " << filename << " completed successfully\n";
  }
  io_uring_cqe_seen(&ring, cqe);

  close(fd);
  io_uring_queue_exit(&ring);
}

template <typename T>
void IOOperations<T>::readAndModify(T shared_ptr, int size,
                                    const char *file_path) {
  int fd = open(file_path, O_RDONLY);
  if (fd < 0) {
    perror("Failed to open file for reading");
    return;
  }

  struct io_uring ring;
  if (io_uring_queue_init(8, &ring, 0) != 0) {
    fprintf(stderr, "io_uring setup failed for reading\n");
    close(fd);
    return;
  }

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_read(sqe, fd, shared_ptr, size * sizeof(int), 0);
  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;
  io_uring_wait_cqe(&ring, &cqe);
  if (cqe->res < 0) {
    fprintf(stderr, "io_uring read failed: %s\n", strerror(-cqe->res));
  } else {
    std::cout << "Read from " << file_path
              << " completed successfully. Modifying data...\n";
    // Call GPU to increment the data
    GPUOperations<T> gpuOps;
    gpuOps.increment(shared_ptr, size);

    // Print the incremented data
    for (int i = 0; i < size; ++i) {
      std::cout << shared_ptr[i] << " ";
    }
    std::cout << std::endl;
  }
  io_uring_cqe_seen(&ring, cqe);

  close(fd);
  io_uring_queue_exit(&ring);
}

#pragma omp end declare target
template <typename T>
void GPUOperations<T>::call_io_function(T shared_ptr, int size,
                                        std::atomic<int> &file_counter) {
  IOOperations<T> ioOps;
  ioOps.process(shared_ptr, size, file_counter);
}

int main() {
  const int BUFFER_SIZE = 1024;
  const int NUM_TEAMS = 4;
  const int THREADS_PER_TEAM = 64;
  std::atomic<int> file_counter(0);

  auto shared_ptr = static_cast<int *>(
      omp_alloc(BUFFER_SIZE * sizeof(int), llvm_omp_target_shared_mem_alloc));

  GPUOperations<int *> gpuOps;
  // IOOperations<int*> ioOps;

  int command = 0;
  while (true) {
    std::cout << "Enter 1 to perform operations, 2 to read from file and "
                 "modify data, any other number to exit: ";
    std::cin >> command;

    if (command == 1) {
      gpuOps.perform(shared_ptr, BUFFER_SIZE, NUM_TEAMS, THREADS_PER_TEAM,
                     file_counter);
    } else if (command == 2) {
      // Ensure to replace "output.bin" with the path to the actual file
      // you want to read from
      IOOperations<int *> ioOps;
      ioOps.readAndModify(shared_ptr, BUFFER_SIZE, "output_4380_0.bin");
    } else {
      std::cout << "Exiting program." << std::endl;
      break;
    }
  }

  return 0;
}
