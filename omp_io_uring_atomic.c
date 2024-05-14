#include <omp.h>
#include <stdio.h>
#include <stdatomic.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    const int N = 64;
    const int BUFFER_SIZE = 1024;  // Size of the ring buffer
    int index = 0;
    // Atomic variable to manage the write position
    atomic_int write_pos = 0;

    // Allocates device-managed memory that is shared between the host and device
    int *shared_ptr = omp_alloc(BUFFER_SIZE * sizeof(int), llvm_omp_target_shared_mem_alloc);

    // Initialize the buffer on the device
#pragma omp target teams distribute parallel for is_device_ptr(shared_ptr)
    for (int i = 0; i < BUFFER_SIZE; ++i) {
        shared_ptr[i] = 0;  // Initialize with zeros
    }

    // Simulation of multiple writes using atomic operation to update the write position
    for (int i = 0; i < N; ++i) {
        int old_pos = atomic_fetch_add(&write_pos, 1) % BUFFER_SIZE;
        shared_ptr[old_pos] = 1;  // Each write sets one element to 1
    }

    // Open file for writing
    int fd = open("output.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to open file");
        omp_free(shared_ptr, llvm_omp_target_shared_mem_alloc);
        return 1;
    }

    // Set up io_uring
    struct io_uring ring;
    if (io_uring_queue_init(8, &ring, 0) != 0) {
        fprintf(stderr, "io_uring setup failed\n");
        close(fd);
        omp_free(shared_ptr, llvm_omp_target_shared_mem_alloc);
        return 1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, fd, shared_ptr, BUFFER_SIZE * sizeof(int), 0);
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
    omp_free(shared_ptr, llvm_omp_target_shared_mem_alloc);

    return 0;
}
