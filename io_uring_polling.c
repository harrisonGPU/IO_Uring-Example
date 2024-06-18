#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define BUFFER_SIZE 1024  // Define the size of the buffer for read operations
#define QUEUE_DEPTH 64    // Define the depth of the io_uring queue

// Structure to encapsulate a FILE stream and an io_uring instance
typedef struct {
    FILE *stream;
    struct io_uring ring;
    struct iovec iov;   // Add an iovec here for reuse
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

// Function to print the status of the io_uring kernel thread
void print_sq_poll_kernel_thread_status() {
    if (system("ps --ppid 2 | grep io_uring-sq") == 0)
        printf("Kernel thread io_uring-sq found running...\n");
    else
        printf("Kernel thread io_uring-sq is not running.\n");
}

// Fopen function to open a file with io_uring setup
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
        mf->iov.iov_base = malloc(BUFFER_SIZE); // Static buffer
        if (mf->iov.iov_base == NULL) {
            perror("Failed to allocate buffer");
            fclose(fp);
            io_uring_queue_exit(&ring);
            free(mf);
            return NULL;
        }
        mf->iov.iov_len = BUFFER_SIZE;          // Length of the static buffer
    } else {
        fclose(fp);
        io_uring_queue_exit(&ring);
    }

    return mf;
}

// Fread function to perform multiple asynchronous reads if needed
size_t my_fread(void *restrict buffer, size_t size, size_t count, my_file *restrict mf) {
    size_t total_bytes = size * count;
    size_t bytes_read = 0;

    while (total_bytes > 0) {
        size_t bytes_to_read = total_bytes > BUFFER_SIZE ? BUFFER_SIZE : total_bytes;
        struct io_uring_sqe *sqe = io_uring_get_sqe(&mf->ring);
        if (!sqe) {
            fprintf(stderr, "Unable to get sqe\n");
            return bytes_read;
        }

        // This is io_uring_prep_readv implement
        sqe->opcode = (__u8)IORING_OP_READV;
        sqe->flags = 0;
        sqe->ioprio = 0;
        sqe->fd = fileno(mf->stream);
        sqe->off = (__u64)0;
        sqe->addr = (unsigned long)&mf->iov;
        sqe->len = (unsigned)1;
        sqe->rw_flags = 0;
        sqe->user_data = 0;
        sqe->buf_index = 0;
        sqe->personality = 0;
        sqe->file_index = 0;
        sqe->__pad2[0] = sqe->__pad2[1] = 0;


        io_uring_submit(&mf->ring);

        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&mf->ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
            break;
        }

        if (cqe->res < 0) {
            fprintf(stderr, "Async readv failed: %s\n", strerror(-cqe->res));
            io_uring_cqe_seen(&mf->ring, cqe);
            break;
        }

        size_t bytes_copied = (size_t)cqe->res > bytes_to_read ? bytes_to_read : (size_t)cqe->res;
        memcpy(buffer, mf->iov.iov_base, bytes_copied); // Copy data to user buffer
        buffer = (char *)buffer + bytes_copied;
        bytes_read += bytes_copied;
        total_bytes -= bytes_copied;

        io_uring_cqe_seen(&mf->ring, cqe);
    }

    return bytes_read / size;
}

// Close the file structure and clean up io_uring resources
void my_fclose(my_file *mf) {
    if (mf) {
        if (mf->stream) {
            fclose(mf->stream);
        }
        free(mf->iov.iov_base); // Free the static buffer
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
        buffer[n] = '\0'; // Null-terminate the buffer to safely print it
        printf("%s", buffer);

        times++;
        if (times == 10)
            break;
    }

    my_fclose(mf);
    print_sq_poll_kernel_thread_status();

    return 0;
}
