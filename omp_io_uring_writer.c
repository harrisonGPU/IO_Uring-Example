#include <omp.h>
#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

int main() {
  const int N = 64;
  int *hst_ptr = omp_alloc(N * sizeof(int), llvm_omp_target_host_mem_alloc);

  // Perform OpenMP operations
  for (int i = 0; i < N; ++i)
    hst_ptr[i] = 2;

#pragma omp target teams distribute parallel for map(tofrom: hst_ptr[0:N])
  for (int i = 0; i < N; ++i)
    hst_ptr[i] -= 1;

  // Convert data to strings and prepare for writing
  char *buffer = malloc(N * 12);
  char *ptr = buffer;
  for (int i = 0; i < N; ++i) {
    ptr += sprintf(ptr, "%d\n", hst_ptr[i]);
  }

  // Initialize io_uring
  struct io_uring ring;
  io_uring_queue_init(8, &ring, 0);
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

  // Setup the write operation
  io_uring_prep_write(sqe, fd, buffer, ptr - buffer, 0);
  io_uring_submit(&ring);

  // Wait for completion
  struct io_uring_cqe *cqe;
  io_uring_wait_cqe(&ring, &cqe);
  io_uring_cqe_seen(&ring, cqe);

  // Clean up
  close(fd);
  io_uring_queue_exit(&ring);
  free(buffer);

  // Summing up results
  int sum = 0;
  for (int i = 0; i < N; ++i)
    sum += hst_ptr[i];
  omp_free(hst_ptr, llvm_omp_target_host_mem_alloc);

  if (sum == N)
    printf("PASS\n");
}
