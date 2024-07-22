#include "my_io.h"
#include <cstring>
#include <iostream>
#include <ctime>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <omp.h>
double perFileTime;

int app_setup_uring(struct submitter *s) {
  memset(s, 0, sizeof(*s));
  struct app_io_sq_ring *sring = &s->sq_ring;
  struct app_io_cq_ring *cring = &s->cq_ring;
  struct io_uring_params p;
  void *sq_ptr, *cq_ptr;

  memset(&p, 0, sizeof(p));
  p.flags |= IORING_SETUP_SQPOLL;
  p.flags |= IORING_SETUP_SQ_AFF;
  p.sq_thread_idle = 20000000;
  s->ring_fd = io_uring_setup(QUEUE_DEPTH, &p);
  if (s->ring_fd < 0) {
    perror("io_uring_setup");
    return 1;
  }

  int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    if (cring_sz > sring_sz) {
      sring_sz = cring_sz;
    }
    cring_sz = sring_sz;
  }

  sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                s->ring_fd, IORING_OFF_SQ_RING);
  if (sq_ptr == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    cq_ptr = sq_ptr;
  } else {
    cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) {
      perror("mmap");
      return 1;
    }
  }

  sring->head = (unsigned *)((char *)sq_ptr + p.sq_off.head);
  sring->tail = (unsigned *)((char *)sq_ptr + p.sq_off.tail);
  sring->ring_mask = (unsigned *)((char *)sq_ptr + p.sq_off.ring_mask);
  sring->ring_entries = (unsigned *)((char *)sq_ptr + p.sq_off.ring_entries);
  sring->flags = (unsigned *)((char *)sq_ptr + p.sq_off.flags);
  sring->array = (unsigned *)((char *)sq_ptr + p.sq_off.array);

  s->sqes = (struct io_uring_sqe *)mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd,
                 IORING_OFF_SQES);
  if (s->sqes == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  cring->head = (unsigned *)((char *)cq_ptr + p.cq_off.head);
  cring->tail = (unsigned *)((char *)cq_ptr + p.cq_off.tail);
  cring->ring_mask = (unsigned *)((char *)cq_ptr + p.cq_off.ring_mask);
  cring->ring_entries = (unsigned *)((char *)cq_ptr + p.cq_off.ring_entries);
  cring->cqes = (struct io_uring_cqe *)((char *)cq_ptr + p.cq_off.cqes);

  return 0;
}

void cat(const char *filename) {
    // init io_uring interface and allocate buffer
    s = static_cast<struct submitter*>(omp_alloc(sizeof(struct submitter), llvm_omp_target_shared_mem_alloc));
    mf = static_cast<my_file *>(omp_alloc(sizeof(my_file), llvm_omp_target_shared_mem_alloc));
    if (app_setup_uring(s)) {
      fprintf(stderr, "Unable to setup uring!\n");
      free(s);
      return;
    }


    my_file *mf = my_fopen(filename, "r");
    if (!mf) {
        perror("Failed to open file.");
        return;
    }

    size_t bytesRead;
    std::unique_ptr<char[]> buffer(new char[4096]);
    auto start = std::clock();

    while ((bytesRead = my_fread(buffer.get(), sizeof(char), sizeof(buffer), mf)) > 0) {
        write(STDOUT_FILENO, buffer.get(), bytesRead);
    }

    auto end = std::clock();
    double cpu_time_used = double(end - start) / CLOCKS_PER_SEC;
    perFileTime += cpu_time_used;

    std::cout << "Completed reading '" << filename << "': Duration = " << cpu_time_used << " seconds, File Size = " << mf->fi->file_sz << " bytes.\n";
    my_fclose(mf);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <filename>\n";
        return 1;
    }

    perFileTime = 0.0;
    auto start = std::clock();

    for (int i = 1; i < argc; i++) {
        cat(argv[i]);
    }

    auto end = std::clock();
    double total_time = double(end - start) / CLOCKS_PER_SEC;

    std::cout << "Total time to read " << (argc - 1) << " files: " << total_time << " seconds\n";
    std::cout << "Average time per read file: " << perFileTime / (argc - 1) << " seconds\n";
    std::cout << "Use io_uring_enter system call times = " << systemTimes << std::endl;

    return 0;
}
