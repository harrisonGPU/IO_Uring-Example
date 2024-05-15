#include <iostream>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <omp.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>

// Global atomic counter for unique file naming
std::atomic<int> file_counter(0);

template<typename T>
class GPUOperations {
public:
    void perform(T shared_ptr, int size, int num_teams, int num_threads_per_team) {
        // Use OpenMP to allocate memory and distribute the workload across multiple GPU threads
        #pragma omp target teams distribute parallel for collapse(2) num_teams(num_teams) thread_limit(num_threads_per_team) map(tofrom: shared_ptr[:size])
        for (int team = 0; team < num_teams; ++team) {
            for (int thread = 0; thread < num_threads_per_team; ++thread) {
                int index = team * num_threads_per_team + thread;
                if (index < size) {
                    shared_ptr[index] = 1; // Each thread sets its designated element to 1
                }
            }
        }
    }
};

template<typename T>
class CPUOperations {
public:
    void process(T shared_ptr, int size) {
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
};

int main() {
    const int BUFFER_SIZE = 1024;
    const int NUM_TEAMS = 4;
    const int THREADS_PER_TEAM = 64;

    auto shared_ptr = static_cast<int*>(omp_alloc(BUFFER_SIZE * sizeof(int), llvm_omp_target_shared_mem_alloc));

    GPUOperations<int*> gpuOps;
    CPUOperations<int*> cpuOps;

    int command = 0;
    while (true) {
        std::cout << "Enter 1 to perform operations, any other number to exit: ";
        std::cin >> command;

        if (command == 1) {
            gpuOps.perform(shared_ptr, BUFFER_SIZE, NUM_TEAMS, THREADS_PER_TEAM);
            cpuOps.process(shared_ptr, BUFFER_SIZE);
        } else {
            std::cout << "Exiting program." << std::endl;
            break;
        }
    }

    omp_free(shared_ptr, llvm_omp_target_shared_mem_alloc);
    return 0;
}
