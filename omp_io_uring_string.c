#include <omp.h>
#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    char *message = "Hello, welcome OpenMP!";
    int message_len = strlen(message) + 1;

    int device_id = omp_get_default_device();

    // Allocate memory accessible by the host (CPU)
    char *buffer = omp_target_alloc(message_len, device_id);
    if (buffer == NULL) {
        fprintf(stderr, "Failed to allocate memory on the target device\n");
        return 1;
    }

    omp_target_memcpy(buffer, message, message_len, 0, 0, device_id, omp_get_initial_device());
    
    // Open file for writing
    int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to open file");
        omp_target_free(buffer, device_id);
        return 1;
    }

    

    // Set up io_uring
    struct io_uring ring;
    io_uring_queue_init(8, &ring, 0);
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, fd, buffer, message_len, 0);
    io_uring_submit(&ring);

    // Wait for completion
    struct io_uring_cqe *cqe;
    io_uring_wait_cqe(&ring, &cqe);
    if (cqe->res < 0) {
        fprintf(stderr, "io_uring write failed: %s\n", strerror(-cqe->res));
    } else {
        printf("Write completed successfully\n");
    }
    io_uring_cqe_seen(&ring, cqe);

    // Clean up
    close(fd);
    io_uring_queue_exit(&ring);
    omp_target_free(buffer, device_id);

    return 0;
}
