#include <iostream>
#include <cstdio>
#include <cstring>
#include <omp.h>
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>

// Flag to indicate when GPU has finished processing
std::atomic<bool> work_ready(false);

class GPUOperations {
public:
    void perform(int* shared_ptr, int size) {
        // Simulate task submission by GPU
        std::cout << "GPU submitting work..." << std::endl;
        #pragma omp target teams distribute parallel for is_device_ptr(shared_ptr)
        for (int i = 0; i < size; ++i) {
            shared_ptr[i] = 1;  // Perform the GPU work
        }
        work_ready = true;  // Indicate work is ready for the CPU to process
    }
};

class CPUOperations {
public:
    void process(int* shared_ptr, int size) {
        // Wait until work is ready
        while (!work_ready) {
            // Busy-waiting; in a real application, consider using condition variables instead
        }

        std::cout << "CPU processing work..." << std::endl;
        int fd = open("output.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("Failed to open file");
            return;
        }

        struct io_uring ring;
        if (io_uring_queue_init(8, &ring, 0) != 0) {
            fprintf(stderr, "io_uring setup failed\n");
            close(fd);
            return;
        }

        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_write(sqe, fd, shared_ptr, size * sizeof(int), 0);
        io_uring_submit(&ring);

        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);
        if (cqe->res < 0) {
            fprintf(stderr, "io_uring write failed: %s\n", strerror(-cqe->res));
        } else {
            std::cout << "Write completed successfully\n";
        }
        io_uring_cqe_seen(&ring, cqe);

        close(fd);
        io_uring_queue_exit(&ring);
    }
};

int main() {
    const int BUFFER_SIZE = 1024;
    int input = 0;
    auto shared_ptr = static_cast<int*>(omp_alloc(BUFFER_SIZE * sizeof(int), llvm_omp_target_shared_mem_alloc));
    
    GPUOperations gpuOps;
    CPUOperations cpuOps;

    while (true) {
        std::cout << "Enter 1 to perform operations, any other number to exit: ";
        std::cin >> input;

        if (input == 1) {
            // Allocate memory on GPU
            gpuOps.perform(shared_ptr, BUFFER_SIZE);
            // Write buffer date to file
            cpuOps.process(shared_ptr, BUFFER_SIZE);
        } else {
            std::cout << "GPU First!\n";
            break; // Exit the loop and end the program
        }
    }

    omp_free(shared_ptr, llvm_omp_target_shared_mem_alloc);
    return 0;
}
