#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <liburing.h>

#define BUFFER_SIZE 4096

int main() {
    struct io_uring ring;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    char buffer[BUFFER_SIZE];
    int ret;

    // Initialize io_uring
    ret = io_uring_queue_init(8, &ring, 0);
    if (ret) {
        fprintf(stderr, "io_uring setup failed: %d\n", ret);
        return 1;
    }

    do {
        // Get a submission queue entry for polling
        sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            fprintf(stderr, "Could not get SQE.\n");
            break;
        }

        // Prepare to poll stdin (fd 0) until it is ready to be read
        io_uring_prep_poll_add(sqe, 0, POLLIN);

        // Submit the poll request
        io_uring_submit(&ring);

        // Wait for poll completion
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret) {
            fprintf(stderr, "Error waiting for poll completion: %d\n", ret);
            break;
        }

        if (cqe->res < 0) {
            fprintf(stderr, "Polling failed: %d\n", cqe->res);
            io_uring_cqe_seen(&ring, cqe);
            break;
        }

        // Mark this poll CQE as seen
        io_uring_cqe_seen(&ring, cqe);

        // Prepare for reading after successful polling
        sqe = io_uring_get_sqe(&ring);
        io_uring_prep_read(sqe, 0, buffer, BUFFER_SIZE, 0); // Read from stdin

        // Submit the read request
        io_uring_submit(&ring);

        // Wait for read completion
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret) {
            fprintf(stderr, "Error waiting for read completion: %d\n", ret);
            break;
        }

        if (cqe->res < 0) {
            fprintf(stderr, "Read failed: %d\n", cqe->res);
            break;
        }

        // Output the read data to stdout
        write(STDOUT_FILENO, buffer, cqe->res);

        // Mark this read CQE as seen
        io_uring_cqe_seen(&ring, cqe);

    } while (cqe->res > 0); // Continue if data was read

    // Cleanup
    io_uring_queue_exit(&ring);

    return 0;
}
