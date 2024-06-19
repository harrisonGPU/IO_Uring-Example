#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
#define QUEUE_DEPTH 16

struct file_info {
  int fd;
  off_t offset;
  char buf[BUFFER_SIZE];
};

void setup_read(struct io_uring *ring, struct file_info *fi,
                struct file_info *files) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe)
    return;

  io_uring_prep_read(sqe, fi->fd, fi->buf, BUFFER_SIZE, fi->offset);
  sqe->user_data = fi - files;
}

int my_fopen(int argc, char *argv[], struct io_uring *ring,
             struct file_info files[QUEUE_DEPTH]) {
  struct io_uring_params params;
  int num_files = argc - 1;

  if (argc < 2) {
    printf("Usage: %s <file1> <file2> ... <fileN>\n", argv[0]);
    return -1;
  }

  memset(&params, 0, sizeof(params));
  params.flags |= IORING_SETUP_SQPOLL;
  params.sq_thread_idle = 1000;

  if (io_uring_queue_init_params(QUEUE_DEPTH, ring, &params)) {
    perror("io_uring_queue_init_params");
    return -1;
  }

  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      perror("open");
      continue;
    }
    files[i - 1].fd = fd;
    files[i - 1].offset = 0;
    setup_read(ring, &files[i - 1], files);
  }

  io_uring_submit(ring);

  return num_files;
}

int my_fread(struct io_uring *ring, struct file_info files[], int *num_files) {
  while (*num_files > 0) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(ring, &cqe);
    if (ret < 0) {
      fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
      break;
    }

    struct file_info *fi = &files[cqe->user_data];
    if (cqe->res < 0) {
      fprintf(stderr, "Async readv failed: %s\n", strerror(-cqe->res));
      (*num_files)--;
    } else if (cqe->res > 0) {
      write(STDOUT_FILENO, fi->buf, cqe->res);
      fi->offset += cqe->res;
      setup_read(ring, fi, files); // Prepare next read
    } else {
      (*num_files)--;
    }

    io_uring_cqe_seen(ring, cqe);
  }

  return 1; // Return status could be used to indicate success or error
            // condition
}

void my_close(struct io_uring *ring, struct file_info files[], int num_files) {
  // Close all open file descriptors
  for (int i = 0; i < num_files; i++) {
    if (files[i].fd != -1) {
      close(files[i].fd);
    }
  }
  // Cleanup the io_uring setup
  io_uring_queue_exit(ring);
}

int main(int argc, char *argv[]) {
  struct io_uring ring;
  struct file_info files[QUEUE_DEPTH];
  int num_files = 0;

  num_files = my_fopen(argc, argv, &ring, files);

  if (num_files <= 0) {
    printf("Fopen failed!\n");
    return 1;
  }

  if (my_fread(&ring, files, &num_files)) {
    // this buffer saved file_info->buf
  }

  my_close(&ring, files, num_files);
  return 0;
}