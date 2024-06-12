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
} my_file;

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

    // Set up io_uring to use SQ polling
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    // Initialize the io_uring queue with the specified parameters
    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
        perror("io_uring_queue_init_params");
        return NULL;
    }

    // Check for root privileges required for setting up SQPOLL
    if (geteuid()) {
        fprintf(stderr, "You need root privileges to run this program.\n");
        io_uring_queue_exit(&ring);
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

    // Retrieve and set file descriptor flags for non-blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        fclose(fp);
        io_uring_queue_exit(&ring);
        return NULL;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        fclose(fp);
        io_uring_queue_exit(&ring);
        return NULL;
    }

    // Register the file descriptor with io_uring
    if (io_uring_register_files(&ring, &fd, 1) < 0) {
        perror("io_uring_register_files");
        fclose(fp);
        io_uring_queue_exit(&ring);
        return NULL;
    }

    // Allocate memory for the custom file structure
    my_file *mf = malloc(sizeof(my_file));
    if (mf) {
        mf->stream = fp;
        mf->ring = ring;
    } else {
        fclose(fp);
        io_uring_queue_exit(&ring);
    }

    return mf;
}

// Fread function to perform asynchronous read using io_uring
size_t my_fread(void *restrict buffer, size_t size, size_t count, my_file *restrict mf) {
    int fd = fileno(mf->stream);
    if (fd < 0) {
        perror("fileno");
        return 0;
    }

    struct iovec iov = {
        .iov_base = buffer,
        .iov_len = size * count
    };

    struct io_uring_sqe *sqe = io_uring_get_sqe(&mf->ring);
    if (!sqe) {
        fprintf(stderr, "Unable to get sqe\n");
        return 0;
    }

    io_uring_prep_readv(sqe, fd, &iov, 1, 0);
    io_uring_submit(&mf->ring);

    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&mf->ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
        return 0;
    }

    if (cqe->res < 0) {
        fprintf(stderr, "Async readv failed: %s\n", strerror(-cqe->res));
        io_uring_cqe_seen(&mf->ring, cqe);
        return 0;
    }

    size_t bytes_read = cqe->res;
    io_uring_cqe_seen(&mf->ring, cqe);
    return bytes_read / size;
}

// Close the file structure and clean up io_uring resources
void my_close(my_file *mf) {
    if (mf) {
        if (mf->stream) {
            fclose(mf->stream);
        }
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
        if (times > 10)
            break;
    }

    my_close(mf);
    print_sq_poll_kernel_thread_status();

    return 0;
}
