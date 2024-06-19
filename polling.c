#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 4096
#define QUEUE_DEPTH 16

struct file_info {
    int fd;
    off_t offset;
    char buf[BUFFER_SIZE];
};

void setup_read(struct io_uring *ring, struct file_info *fi, struct file_info *files);

int main(int argc, char *argv[]) {
    struct io_uring ring;
    struct io_uring_params params;
    struct file_info files[QUEUE_DEPTH];
    int num_files = argc - 1;
    int first_submission = 1;

    if (argc < 2) {
        printf("Usage: %s <file1> <file2> ... <fileN>\n", argv[0]);
        return 1;
    }

    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 10000;

    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params)) {
        perror("io_uring_queue_init_params");
        return 1;
    }

    // Open files and setup first read
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            perror("open");
            continue;
        }
        files[i-1].fd = fd;
        files[i-1].offset = 0;
        setup_read(&ring, &files[i-1], files);
    }

    if (first_submission) {
        io_uring_submit(&ring);
        first_submission = 0;
    }

    // Main event loop
    while (num_files > 0) {
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
            break;
        }

        struct file_info *fi = &files[cqe->user_data];
        if (cqe->res < 0) {
            fprintf(stderr, "Async readv failed: %s\n", strerror(-cqe->res));
            num_files--;
        } else if (cqe->res > 0) {
            write(STDOUT_FILENO, fi->buf, cqe->res);
            fi->offset += cqe->res;
            setup_read(&ring, fi, files); // Prepare next read
        } else {
            close(fi->fd);
            num_files--;
        }

        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);
    return 0;
}

void setup_read(struct io_uring *ring, struct file_info *fi, struct file_info *files) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;

    io_uring_prep_read(sqe, fi->fd, fi->buf, BUFFER_SIZE, fi->offset);
    sqe->user_data = fi - files; // Calculate index based on pointer arithmetic
}
