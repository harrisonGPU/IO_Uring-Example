#include <omp.h>
#include <stdio.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    const char *message = "Hello, welcome OpenMP!";
    int message_len = strlen(message) + 1;  // +1 to include the null terminator

    // Allocate memory accessible by the host (CPU)
    char *buffer = omp_alloc(message_len, llvm_omp_target_shared_mem_alloc);
    if (buffer == NULL) {
        fprintf(stderr, "Failed to allocate memory on the target device\n");
        return 1;
    }

    // Copy the message into the buffer
    strcpy(buffer, message);

    // Open file for writing
    int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to open file");
        omp_free(buffer, llvm_omp_target_shared_mem_alloc);
        return 1;
    }

    // Set up io_uring
    struct io_uring ring;
    if (io_uring_queue_init(8, &ring, 0) != 0) {
        fprintf(stderr, "Failed to initialize io_uring\n");
        close(fd);
        omp_free(buffer, llvm_omp_target_shared_mem_alloc);
        return 1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "Failed to get submission queue entry\n");
        close(fd);
        io_uring_queue_exit(&ring);
        omp_free(buffer, llvm_omp_target_shared_mem_alloc);
        return 1;
    }

    io_uring_prep_write(sqe, fd, buffer, message_len, 0);
    io_uring_submit(&ring);

    // Wait for completion
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&ring, &cqe) != 0) {
        fprintf(stderr, "Failed to wait for completion\n");
        close(fd);
        io_uring_queue_exit(&ring);
        omp_free(buffer, llvm_omp_target_shared_mem_alloc);
        return 1;
    }

    if (cqe->res < 0) {
        fprintf(stderr, "io_uring write failed: %s\n", strerror(-cqe->res));
    } else {
        printf("Write completed successfully\n");
    }

    io_uring_cqe_seen(&ring, cqe);

    // Clean up
    close(fd);
    io_uring_queue_exit(&ring);
    omp_free(buffer, llvm_omp_target_shared_mem_alloc);

    return 0;
}
