#include <fcntl.h>
#include <inttypes.h> // For PRIdMAX
#include <linux/fs.h>
#include <linux/io_uring.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h> // For off_t
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define FILE_BLOCK_SIZE                                                        \
  4096 // Use a clear block size that doesn't conflict with any system
       // definitions

int io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                   unsigned flags) {
  return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, NULL,
                 0);
}

void die(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
  struct io_uring_params params;
  struct io_uring_sqe *sqe;
  struct io_uring_cqe *cqe;
  void *ring_ptr;
  unsigned *sq_head, *sq_tail, *cq_head, *cq_tail;
  struct iovec iov;
  int src_fd, dst_fd, ring_fd;
  off_t file_size;
  struct stat st;

  if (argc != 3) {
    fprintf(stderr, "Usage: %s <source_file> <dest_file>\n", argv[0]);
    exit(1);
  }

  src_fd = open(argv[1], O_RDONLY);
  if (src_fd < 0)
    die("Opening source file failed");

  if (fstat(src_fd, &st) < 0)
    die("Failed to get file size");
  file_size = st.st_size;

  dst_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst_fd < 0)
    die("Opening destination file failed");

  memset(&params, 0, sizeof(params));
  ring_fd = io_uring_setup(2, &params);
  if (ring_fd < 0)
    die("io_uring setup failed");

  ring_ptr = mmap(0, params.sq_off.array + params.sq_entries * sizeof(unsigned),
                  PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
                  IORING_OFF_SQ_RING);
  if (ring_ptr == MAP_FAILED)
    die("Failed to mmap");

  sqe = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
             IORING_OFF_SQES);
  if (sqe == MAP_FAILED)
    die("Failed to mmap SQEs");

  sq_head = ring_ptr + params.sq_off.head;
  sq_tail = ring_ptr + params.sq_off.tail;
  cq_head = ring_ptr + params.cq_off.head;
  cq_tail = ring_ptr + params.cq_off.tail;

  char *buffer = malloc(FILE_BLOCK_SIZE);
  if (!buffer)
    die("Failed to allocate buffer");

  off_t offset = 0;
  while (offset < file_size) {
    ssize_t bytes_to_read = (file_size - offset) > FILE_BLOCK_SIZE
                                ? FILE_BLOCK_SIZE
                                : (file_size - offset);

    iov.iov_base = buffer;
    iov.iov_len = bytes_to_read;

    // Prepare and submit read SQE
    memset(sqe, 0, sizeof(*sqe));
    sqe->fd = src_fd;
    sqe->opcode = IORING_OP_READV;
    sqe->addr = (unsigned long)&iov;
    sqe->len = 1;
    sqe->off = offset;
    sqe->user_data = 1;
    *sq_tail = (*sq_tail + 1) % params.sq_entries;
    io_uring_enter(ring_fd, 1, 1, 0);

    // Wait for read completion
    while (*cq_head == *cq_tail);
    cqe = ring_ptr + params.cq_off.cqes +
          (*cq_head % params.cq_entries) * sizeof(*cqe);
    if (cqe->res < 0)
      die("Read failed");
    int bytes_read = cqe->res;
    (*cq_head)++;

    // Prepare and submit write SQE
    memset(sqe, 0, sizeof(*sqe));
    sqe->fd = dst_fd;
    sqe->opcode = IORING_OP_WRITEV;
    sqe->addr = (unsigned long)&iov;
    sqe->len = 1;
    sqe->off = offset;
    sqe->user_data = 2;
    *sq_tail = (*sq_tail + 1) % params.sq_entries;
    io_uring_enter(ring_fd, 1, 1, 0);

    // Wait for write completion
    while (*cq_head == *cq_tail);
    cqe = ring_ptr + params.cq_off.cqes +
          (*cq_head % params.cq_entries) * sizeof(*cqe);
    if (cqe->res < bytes_read)
      die("Write failed");
    (*cq_head)++;

    offset += bytes_read;
  }

  free(buffer);
  close(src_fd);
  close(dst_fd);
  munmap(ring_ptr, params.sq_off.array + params.sq_entries * sizeof(unsigned));
  munmap(sqe, params.sq_entries * sizeof(struct io_uring_sqe));

  return 0;
}
