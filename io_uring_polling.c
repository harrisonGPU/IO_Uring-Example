#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define BUFFER_SIZE 1024

size_t my_fread(void *restrict buffer, size_t size, size_t count, FILE *restrict stream) {
    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;  // SQPOLL线程空闲时间，以毫秒为单位

    if (io_uring_queue_init_params(32, &ring, &params) < 0) {
        perror("io_uring_queue_init_params");
        return 0;
    }

    int fd = fileno(stream);
    if (fd < 0) {
        perror("fileno");
        return 0;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "Unable to get sqe\n");
        io_uring_queue_exit(&ring);
        return 0;
    }

    struct iovec iov = {
        .iov_base = buffer,
        .iov_len = size * count
    };

    io_uring_prep_readv(sqe, fd, &iov, 1, 0);

    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
        io_uring_queue_exit(&ring);
        return 0;
    }

    if (cqe->res < 0) {
        fprintf(stderr, "Async readv failed.\n");
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
    FILE *fp = fopen("CMakeLists.txt", "r");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    char buffer[BUFFER_SIZE];
    size_t n = 0;

    int times = 0;
    while ((n = my_fread(buffer, sizeof(char), BUFFER_SIZE, fp))) {
        buffer[n] = '\0'; // Null-terminate the buffer to safely print it
        printf("%s", buffer);
        
        times++;
        if(times > 2)
            break;
    }

    fclose(fp);
    return 0;
}
