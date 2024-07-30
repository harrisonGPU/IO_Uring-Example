#ifndef M_IO_H
#define M_IO_H

#include <atomic>
#include <cstdio> // For FILE and off_t
#include <linux/io_uring.h>

#define QUEUE_DEPTH 256
#define BLOCK_SZ 4096

inline void read_barrier() {
    std::atomic_thread_fence(std::memory_order_acquire);
}

inline void write_barrier() {
    std::atomic_thread_fence(std::memory_order_release);
}

struct io_uring_params;
struct io_uring_sqe;
struct io_uring_cqe;

struct app_io_sq_ring {
    unsigned *head;
    unsigned *tail;
    unsigned *ring_mask;
    unsigned *ring_entries;
    unsigned *flags;
    unsigned *array;
};

struct app_io_cq_ring {
    unsigned *head;
    unsigned *tail;
    unsigned *ring_mask;
    unsigned *ring_entries;
    struct io_uring_cqe *cqes;
};

struct submitter {
    int ring_fd;
    app_io_sq_ring sq_ring;
    struct io_uring_sqe *sqes;
    app_io_cq_ring cq_ring;
};

struct iovc {
    void *buffer; // Pointer to data
    size_t buffer_size; // Length of data
};

struct file_info {
  off_t file_sz;
  struct iovc iovecs[]; /* Referred by readv/writev */
};

struct my_file {
    int fd;
    int blocks;
    int current_block;
    int isfirst;
    size_t current_offset;
    submitter *s;
    file_info *fi_read;
    file_info *fi;
};

// Global value
extern int systemTimes;

int io_uring_setup(unsigned entries, io_uring_params *p);
int io_uring_enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags);
int io_uring_register(unsigned int fd, unsigned int opcode, const void *arg, unsigned int nr_args);
off_t get_file_size(FILE *file);
void update_file_size(my_file *mf);
int app_setup_uring(submitter *s);
my_file *my_fopen(const char *filename, const char *mode);
bool submitRequest();
size_t my_fread(void *ptr, size_t size, size_t count, my_file *mf);
void my_fclose(my_file *mf);

#endif // M_IO_H
