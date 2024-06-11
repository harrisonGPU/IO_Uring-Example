#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define BUFFER_SIZE 1024
#define QUEUE_DEPTH 64

void print_sq_poll_kernel_thread_status() {
    if (system("ps --ppid 2 | grep io_uring-sq") == 0)
        printf("Kernel thread io_uring-sq found running...\n");
    else
        printf("Kernel thread io_uring-sq is not running.\n");
}

size_t my_fread(void *restrict buffer, size_t size, size_t count, FILE *restrict stream) {
    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
        perror("io_uring_queue_init_params");
        return 0;
    }

    int fd = fileno(stream);
    if (fd < 0) {
        perror("fileno");
        io_uring_queue_exit(&ring);
        return 0;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        io_uring_queue_exit(&ring);
        return 0;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        io_uring_queue_exit(&ring);
        return 0;
    }

    if (io_uring_register_files(&ring, &fd, 1) < 0) {
        perror("io_uring_register_files");
        io_uring_queue_exit(&ring);
        return 0;
    }

    struct iovec iov = {
        .iov_base = buffer,
        .iov_len = size * count
    };

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "Unable to get sqe\n");
        io_uring_queue_exit(&ring);
        return 0;
    }

    io_uring_prep_readv(sqe, 0, &iov, 1, 0);
    sqe->flags |= IOSQE_FIXED_FILE;

    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
        io_uring_queue_exit(&ring);
        return 0;
    }

    if (cqe->res < 0) {
        fprintf(stderr, "Async readv failed: %s\n", strerror(-cqe->res));
        io_uring_cqe_seen(&ring, cqe);
        io_uring_queue_exit(&ring);
        return 0;
    }

    size_t bytes_read = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);

    return bytes_read / size;
}

int main(int argc, char *argv[]) {
    if (geteuid()) {
        fprintf(stderr, "You need root privileges to run this program.\n");
        return 1;
    }

    print_sq_poll_kernel_thread_status();

    FILE *fp = fopen("CMakeLists.txt", "r");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    char buffer[BUFFER_SIZE];
    size_t n = 0;

    int times = 0;
    while ((n = my_fread(buffer, sizeof(char), BUFFER_SIZE, fp)) > 0) {
        buffer[n] = '\0'; // Null-terminate the buffer to safely print it
        printf("%s", buffer);

        times++;
        if (times > 10)
            break;
    }

    fclose(fp);

    print_sq_poll_kernel_thread_status();

    return 0;
}
