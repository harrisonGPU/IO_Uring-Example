#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <liburing.h>

#define N 100

int main() {
    struct io_uring ring;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    int fd, ret;
    char *buffer;

    // Allocate memory for the buffer to store string representations of integers
    buffer = malloc(N * 12); // Assuming each integer can be up to 11 characters long (+1 for '\n')
    if (!buffer) {
        perror("Failed to allocate buffer");
        return 1;
    }

    // Fill the buffer with integers converted to strings, each followed by a newline
    char *ptr = buffer;
    for (int i = 0; i < N; i++) {
        ptr += sprintf(ptr, "%d\n", i); // Store integer and advance the pointer
    }

    // Open output file
    fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to open file");
        free(buffer);
        return 1;
    }

    // Initialize io_uring
    if (io_uring_queue_init(8, &ring, 0) != 0) {
        perror("io_uring setup failed");
        close(fd);
        free(buffer);
        return 1;
    }

    // Get a submission queue entry
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "Could not get SQE.\n");
        io_uring_queue_exit(&ring);
        close(fd);
        free(buffer);
        return 1;
    }

    // Prepare the write operation
    io_uring_prep_write(sqe, fd, buffer, ptr - buffer, 0); // Write the entire buffer

    // Submit the write request
    io_uring_submit(&ring);

    // Wait for completion
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "Error waiting for completion: %d\n", ret);
        io_uring_queue_exit(&ring);
        close(fd);
        free(buffer);
        return 1;
    }

    // Check result
    if (cqe->res < 0) {
        fprintf(stderr, "Write failed: %d\n", cqe->res);
        io_uring_queue_exit(&ring);
        close(fd);
        free(buffer);
        return 1;
    }

    // Mark this CQE as seen
    io_uring_cqe_seen(&ring, cqe);

    // Cleanup
    io_uring_queue_exit(&ring);
    close(fd);
    free(buffer);

    return 0;
}
