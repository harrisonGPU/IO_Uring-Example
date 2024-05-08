#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <liburing.h>

#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    struct io_uring ring;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    int fd;
    off_t offset = 0;
    ssize_t read_bytes;

    if (argc < 2) {
        printf("Usage: %s <file>\n", argv[0]);
        return 1;
    }

    // Open file
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("Failed to open file");
        return 1;
    }

    // Initialize io_uring
    if (io_uring_queue_init(8, &ring, 0) < 0) {
        perror("io_uring_queue_init failed");
        close(fd);
        return 1;
    }

    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        perror("Failed to allocate buffer");
        close(fd);
        io_uring_queue_exit(&ring);
        return 1;
    }

    while (1) {
        // Get a submission queue entry
        sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            fprintf(stderr, "Could not get SQE.\n");
            break;
        }

        // Prepare the readv operation
        io_uring_prep_read(sqe, fd, buffer, BUFFER_SIZE, offset);

        // Submit it to the queue
        if (io_uring_submit(&ring) < 0) {
            fprintf(stderr, "io_uring_submit failed\n");
            break;
        }

        // Wait for completion
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed\n");
            break;
        }

        // Check how many bytes were read
        read_bytes = cqe->res;
        if (read_bytes <= 0) {
            break;
        }

        // Write the buffer to standard output
        write(STDOUT_FILENO, buffer, read_bytes);

        // Update offset
        offset += read_bytes;

        // Mark this CQE as seen
        io_uring_cqe_seen(&ring, cqe);
    }

    free(buffer);
    close(fd);
    io_uring_queue_exit(&ring);
    return 0;
}
