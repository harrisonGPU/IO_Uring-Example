#ifndef MY_IO_H
#define MY_IO_H

#include <stdio.h>
#include <sys/types.h> // For off_t
#include <sys/uio.h>
#include <linux/io_uring.h>

#define QUEUE_DEPTH 256
#define BLOCK_SZ 4096

#define read_barrier() __asm__ __volatile__("" ::: "memory")
#define write_barrier() __asm__ __volatile__("" ::: "memory")

#define io_uring_smp_store_release(p, v)                                       \
  atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release)
#define io_uring_smp_load_acquire(p)                                           \
  atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)

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
  struct app_io_sq_ring sq_ring;
  struct io_uring_sqe *sqes;
  struct app_io_cq_ring cq_ring;
};

struct file_info {
  off_t file_sz;
  struct iovec iovecs[]; /* Referred by readv/writev */
};

typedef struct {
  FILE *fp;
  int blocks;
  int current_block;
  int isfirst;
  size_t current_offset;
  struct submitter *s;
  struct file_info *fi_read;
  struct file_info *fi;
} my_file;

// Define the global structure
#define MAX_BUFFER_SIZE 10 * 1024 * 1024

typedef struct {
  char *buffer;
  size_t buffer_pos;
  size_t buffer_size;
} BufferManager;

extern int systemTimes;

int io_uring_setup(unsigned entries, struct io_uring_params *p);
int io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags);
int io_uring_register(unsigned int fd, unsigned int opcode, const void *arg,
                      unsigned int nr_args);
off_t get_file_size(FILE *file);
int app_setup_uring(struct submitter *s);
my_file *my_fopen(const char *filename, const char *mode);
size_t my_fread(void *ptr, size_t size, size_t count, my_file *mf);

#endif // MY_IO_H
