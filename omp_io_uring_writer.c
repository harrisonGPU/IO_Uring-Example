#include <omp.h>
#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

int main() {
  const int N = 64;
  int *hst_ptr = omp_target_alloc(N * sizeof(int), omp_get_default_device());
  if (hst_ptr == NULL) {
    fprintf(stderr, "Failed to allocate memory on the device\n");
    return 1;
  }
  // Use the data in an OpenMP target region
#pragma omp target teams distribute parallel for map(tofrom: hst_ptr[0:N])
  for (int i = 0; i < N; ++i)
    hst_ptr[i] = 1;

  // Convert data to strings for human-readable output
  char *buffer = omp_target_alloc(N * 12, omp_get_default_device());
  char *ptr = buffer;
  if (buffer == NULL) {
    fprintf(stderr, "Failed to allocate memory for buffer\n");
    omp_target_free(hst_ptr, omp_get_default_device());
    return 1;
  }
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

  // Sum up results and check
  int sum = 0;
  for (int i = 0; i < N; ++i)
    sum += hst_ptr[i];
  omp_target_free(hst_ptr, omp_get_default_device());

  if (sum == N)
    printf("PASS\n");
}

