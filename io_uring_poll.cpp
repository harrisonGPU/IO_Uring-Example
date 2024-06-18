#include <iostream>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <liburing.h>
#include <atomic>

#define BUFFER_SIZE 1024  // Define the size of the buffer for read operations
#define QUEUE_DEPTH 64    // Define the depth of the io_uring queue

// Structure to encapsulate a FILE stream and an io_uring instance
struct MyFile {
    std::FILE *stream;
    struct io_uring ring;
    struct iovec iov;
};

template <typename T>
static inline void io_uring_smp_store_release_my(T *p, T v)
{
	std::atomic_store_explicit(reinterpret_cast<std::atomic<T> *>(p), v,
				   std::memory_order_release);
}
static inline void io_uring_cq_advance_my(struct io_uring *ring,
				       unsigned nr)
{
	if (nr) {
		struct io_uring_cq *cq = &ring->cq;

		/*
		 * Ensure that the kernel only sees the new value of the head
		 * index after the CQEs have been read.
		 */
		io_uring_smp_store_release_my(cq->khead, *cq->khead + nr);
	}
}

static inline void io_uring_cqe_seen_my(struct io_uring *ring,
				     struct io_uring_cqe *cqe)
{
	if (cqe)
		io_uring_cq_advance_my(ring, 1);
}

// Function to open a file with io_uring setup
MyFile *my_fopen(const char *filename, const char *mode) {
    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));

    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;
    struct io_uring ring;

    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
        perror("io_uring_queue_init_params");
        return nullptr;
    }

    std::FILE *fp = std::fopen(filename, mode);
    if (!fp) {
        perror("fopen");
        io_uring_queue_exit(&ring);
        return nullptr;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        perror("fileno");
        std::fclose(fp);
        io_uring_queue_exit(&ring);
        return nullptr;
    }

    if (io_uring_register_files(&ring, &fd, 1) < 0) {
        perror("io_uring_register_files");
        std::fclose(fp);
        io_uring_queue_exit(&ring);
        return nullptr;
    }

    auto *mf = new MyFile{fp, ring};
    mf->iov.iov_base = std::malloc(BUFFER_SIZE); // Allocate buffer
    if (!mf->iov.iov_base) {
        perror("Failed to allocate buffer");
        std::fclose(fp);
        io_uring_queue_exit(&ring);
        delete mf;
        return nullptr;
    }
    mf->iov.iov_len = BUFFER_SIZE;

    return mf;
}

// Function to perform multiple asynchronous reads if needed
size_t my_fread(void *buffer, size_t size, size_t count, MyFile *mf) {
    size_t total_bytes = size * count;
    size_t bytes_read = 0;

    while (total_bytes > 0) {
        size_t bytes_to_read = total_bytes > BUFFER_SIZE ? BUFFER_SIZE : total_bytes;
        struct io_uring_sqe *sqe = io_uring_get_sqe(&mf->ring);
        if (!sqe) {
            std::cerr << "Unable to get sqe\n";
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
            std::cerr << "Error waiting for completion: " << strerror(-ret) << '\n';
            break;
        }

        if (cqe->res < 0) {
          std::cerr << "Async readv failed: " << strerror(-cqe->res) << '\n';

          // Implement io_uring_cqe_seen
          if (cqe != nullptr) {
            struct io_uring_cq *cq = &mf->ring.cq;
            std::atomic_store_explicit(
                reinterpret_cast<std::atomic<unsigned> *>(cq->khead),
                *cq->khead + 1, std::memory_order_release);
          }
          break;
        }

        size_t bytes_copied = (size_t)cqe->res > bytes_to_read ? bytes_to_read : (size_t)cqe->res;
        std::memcpy(buffer, mf->iov.iov_base, bytes_copied); // Copy data to user buffer
        buffer = static_cast<char *>(buffer) + bytes_copied;
        bytes_read += bytes_copied;
        total_bytes -= bytes_copied;

        // Implement io_uring_cqe_seen
        if (cqe != nullptr) {
          struct io_uring_cq *cq = &mf->ring.cq;
          std::atomic_store_explicit(
              reinterpret_cast<std::atomic<unsigned> *>(cq->khead),
              *cq->khead + 1, std::memory_order_release);
        }
    }

    return bytes_read / size;
}

// Close the file structure and clean up io_uring resources
void my_fclose(MyFile *mf) {
    if (mf) {
        if (mf->stream) {
            std::fclose(mf->stream);
        }
        std::free(mf->iov.iov_base);
        io_uring_queue_exit(&mf->ring);
        delete mf;
    }
}

int main() {
    MyFile *mf = my_fopen("CMakeLists.txt", "r");
    if (!mf) {
        return 1;
    }

    char buffer[BUFFER_SIZE];
    size_t n = 0;
    int times = 0;

    while ((n = my_fread(buffer, sizeof(char), BUFFER_SIZE, mf)) > 0) {
        buffer[n] = '\0';
        std::cout << buffer;

        times++;
        if (times == 10)
            break;
    }

    my_fclose(mf);

    return 0;
}
