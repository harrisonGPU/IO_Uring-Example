#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define BUFFER_SIZE 1024  // Define the size of the buffer for read operations
#define QUEUE_DEPTH 64    // Define the depth of the io_uring queue

typedef struct {
    FILE *stream;
    struct io_uring ring;
    struct iovec iov;
    int async_initiated;  // Add a flag to track if the async read has been initiated
} my_file;

static inline void io_uring_prep_rw_my(int op, struct io_uring_sqe *sqe, int fd,
                                        const void *addr, unsigned len,
                                        __u64 offset)
{
    sqe->opcode = (__u8) op;
    sqe->flags = 0;
    sqe->ioprio = 0;
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = (unsigned long) addr;
    sqe->len = len;
    sqe->rw_flags = 0;
    sqe->user_data = 0;
    sqe->buf_index = 0;
    sqe->personality = 0;
    sqe->file_index = 0;
    sqe->__pad2[0] = sqe->__pad2[1] = 0;
}

static inline void io_uring_prep_readv_my(struct io_uring_sqe *sqe, int fd,
                                          const struct iovec *iovecs,
                                          unsigned nr_vecs, __u64 offset)
{
    io_uring_prep_rw_my(IORING_OP_READV, sqe, fd, iovecs, nr_vecs, offset);
}

my_file *my_fopen(const char *filename, const char *mode) {
    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
        perror("io_uring_queue_init_params");
        return NULL;
    }

    FILE *fp = fopen(filename, mode);
    if (!fp) {
        perror("fopen");
        io_uring_queue_exit(&ring);
        return NULL;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        perror("fileno");
        fclose(fp);
        io_uring_queue_exit(&ring);
        return NULL;
    }

    if (io_uring_register_files(&ring, &fd, 1) < 0) {
        perror("io_uring_register_files");
        fclose(fp);
        io_uring_queue_exit(&ring);
        return NULL;
    }

    my_file *mf = malloc(sizeof(my_file));
    if (mf) {
        mf->stream = fp;
        mf->ring = ring;
        mf->iov.iov_base = malloc(BUFFER_SIZE);
        if (mf->iov.iov_base == NULL) {
            perror("Failed to allocate buffer");
            fclose(fp);
            io_uring_queue_exit(&ring);
            free(mf);
            return NULL;
        }
        mf->iov.iov_len = BUFFER_SIZE;
        mf->async_initiated = 0;  // Initialize the async flag
    } else {
        fclose(fp);
        io_uring_queue_exit(&ring);
    }

    return mf;
}

size_t my_fread(void *restrict buffer, size_t size, size_t count, my_file *restrict mf) {
    size_t total_bytes = size * count;
    size_t bytes_read = 0;

    if (!mf->async_initiated) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&mf->ring);
        if (!sqe) {
            fprintf(stderr, "Unable to get sqe\n");
            return bytes_read;
        }
        io_uring_prep_readv_my(sqe, fileno(mf->stream), &mf->iov, 1, 0);
        io_uring_submit(&mf->ring);
        mf->async_initiated = 1;  // Set the flag as the async read has been initiated

        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&mf->ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
            return bytes_read;
        }

        if (cqe->res < 0) {
            fprintf(stderr, "Async readv failed: %s\n", strerror(-cqe->res));
            io_uring_cqe_seen(&mf->ring, cqe);
            return bytes_read;
        }

        size_t bytes_copied = (size_t)cqe->res;
        memcpy(buffer, mf->iov.iov_base, bytes_copied);
        buffer = (char *)buffer + bytes_copied;
        bytes_read += bytes_copied;
        total_bytes -= bytes_copied;
        io_uring_cqe_seen(&mf->ring, cqe);
    }

    // Perform remaining reads synchronously if needed
    while (total_bytes > 0) {
        size_t bytes_to_read = total_bytes > BUFFER_SIZE ? BUFFER_SIZE : total_bytes;
        size_t result = fread(buffer, 1, bytes_to_read, mf->stream);
        if (result < bytes_to_read) {
            if (feof(mf->stream)) break;
            if (ferror(mf->stream)) {
                perror("fread");
                break;
            }
        }
        buffer = (char *)buffer + result;
        bytes_read += result;
        total_bytes -= result;
    }

    return bytes_read / size;
}

void my_fclose(my_file *mf) {
    if (mf) {
        if (mf->stream) {
            fclose(mf->stream);
        }
        free(mf->iov.iov_base);
        io_uring_queue_exit(&mf->ring);
        free(mf);
    }
}

int main(int argc, char *argv[]) {
    my_file *mf = my_fopen("CMakeLists.txt", "r");
    if (!mf) {
        return 1;
    }

    char buffer[BUFFER_SIZE];
    size_t n = 0;
    int times = 0;

    while ((n = my_fread(buffer, sizeof(char), BUFFER_SIZE, mf)) > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);

        times++;
        if (times == 10)
            break;
    }

    my_fclose(mf);

    return 0;
}