#include <fcntl.h>
#include <inttypes.h> // For PRIdMAX
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h> // For off_t
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include <linux/io_uring.h>

#define QUEUE_DEPTH 256
#define BLOCK_SZ 4096
int systemTimes;
/* This is x86 specific */
#define read_barrier() __asm__ __volatile__("" ::: "memory")
#define write_barrier() __asm__ __volatile__("" ::: "memory")

/* Macros for barriers needed by io_uring */
#define io_uring_smp_store_release(p, v)                                       \
  std::atomic_store_explicit(reinterpret_cast<std::atomic<unsigned>*>(p), (v), std::memory_order_release)
#define io_uring_smp_load_acquire(p)                                           \
  std::atomic_load_explicit(reinterpret_cast<std::atomic<unsigned>*>(p), std::memory_order_acquire)

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

struct file_info {
  off_t file_sz;
  std::vector<iovec> iovecs; /* Referred by readv/writev */
};

struct my_file {
  FILE *fp;
  int blocks;
  int current_block;
  int isfirst;
  size_t current_offset;
  std::unique_ptr<submitter> s;
  std::unique_ptr<file_info> fi_read;
  std::unique_ptr<file_info> fi;
};

// Define the global structure
#define MAX_BUFFER_SIZE 10 * 1024 * 1024

struct BufferManager {
  std::unique_ptr<char[]> buffer;
  size_t buffer_pos;
  size_t buffer_size;
};

BufferManager buffer_manager;

/*
 * This code is written in the days when io_uring-related system calls are not
 * part of standard C libraries. So, we roll our own system call wrapper
 * functions.
 * */

int io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return static_cast<int>(syscall(__NR_io_uring_setup, entries, p));
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags) {
  return static_cast<int>(syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                      flags, nullptr, 0));
}

int io_uring_register(unsigned int fd, unsigned int opcode, const void *arg,
                      unsigned int nr_args) {
  return static_cast<int>(syscall(__NR_io_uring_register, fd, opcode, arg, nr_args));
}

off_t get_file_size(FILE *file) {
  struct stat st;
  int fd = fileno(file);

  if (fd == -1) {
    perror("fileno");
    return -1;
  }

  if (fstat(fd, &st) < 0) {
    perror("fstat");
    return -1;
  }

  if (S_ISBLK(st.st_mode)) {
    unsigned long long bytes;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      perror("ioctl");
      return -1;
    }
    return bytes;
  } else if (S_ISREG(st.st_mode)) {
    return st.st_size;
  }

  return -1;
}

int app_setup_uring(submitter *s) {
  memset(s, 0, sizeof(*s));
  struct app_io_sq_ring *sring = &s->sq_ring;
  struct app_io_cq_ring *cring = &s->cq_ring;
  struct io_uring_params p;
  void *sq_ptr, *cq_ptr;

  memset(&p, 0, sizeof(p));
  p.flags |= IORING_SETUP_SQPOLL;
  p.flags |= IORING_SETUP_SQ_AFF;
  p.sq_thread_idle = 2000000;
  // p.sq_thread_cpu = 4;
  s->ring_fd = io_uring_setup(QUEUE_DEPTH, &p);
  if (s->ring_fd < 0) {
    perror("io_uring_setup");
    return 1;
  }

  int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    if (cring_sz > sring_sz) {
      sring_sz = cring_sz;
    }
    cring_sz = sring_sz;
  }

  sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                s->ring_fd, IORING_OFF_SQ_RING);
  if (sq_ptr == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    cq_ptr = sq_ptr;
  } else {
    /* Map in the completion queue ring buffer in older kernels separately */
    cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) {
      perror("mmap");
      return 1;
    }
  }

  sring->head = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(sq_ptr) + p.sq_off.head));
  sring->tail = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(sq_ptr) + p.sq_off.tail));
  sring->ring_mask = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(sq_ptr) + p.sq_off.ring_mask));
  sring->ring_entries = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(sq_ptr) + p.sq_off.ring_entries));
  sring->flags = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(sq_ptr) + p.sq_off.flags));
  sring->array = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(sq_ptr) + p.sq_off.array));

  s->sqes = static_cast<struct io_uring_sqe*>(mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd,
                 IORING_OFF_SQES));
  if (s->sqes == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  cring->head = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(cq_ptr) + p.cq_off.head));
  cring->tail = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(cq_ptr) + p.cq_off.tail));
  cring->ring_mask = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(cq_ptr) + p.cq_off.ring_mask));
  cring->ring_entries = static_cast<unsigned*>(static_cast<void*>(static_cast<char*>(cq_ptr) + p.cq_off.ring_entries));
  cring->cqes = static_cast<struct io_uring_cqe*>(static_cast<void*>(static_cast<char*>(cq_ptr) + p.cq_off.cqes));

  return 0;
}

std::unique_ptr<my_file> my_fopen(const char *filename, const char *mode) {
  auto s = std::make_unique<submitter>();
  if (app_setup_uring(s.get())) {
    std::cerr << "Unable to setup uring!\n";
    return nullptr;
  }

  FILE *fp = fopen(filename, mode);
  if (!fp) {
    std::cerr << "Fopen Failed!\n";
    return nullptr;
  }
  int fd = fileno(fp);

  if (fd < 0) {
    std::cerr << "Invalid file descriptor.\n";
    return nullptr;
  }

  struct app_io_sq_ring *sring = &s->sq_ring;
  unsigned index = 0, current_block = 0, tail = 0, next_tail = 0;

  off_t file_sz = get_file_size(fp);
  if (file_sz < 0)
    return nullptr;
  off_t bytes_remaining = file_sz;
  int blocks = file_sz / BLOCK_SZ + (file_sz % BLOCK_SZ ? 1 : 0);

  auto fi = std::make_unique<file_info>();
  fi->file_sz = file_sz;
  fi->iovecs.resize(blocks);

  for (int i = 0; i < blocks; ++i) {
    size_t bytes_to_read = std::min(static_cast<off_t>(BLOCK_SZ), bytes_remaining);
    void *buf;
    if (posix_memalign(&buf, BLOCK_SZ, bytes_to_read)) {
      std::cerr << "Memory alignment error.\n";
      return nullptr;
    }
    fi->iovecs[i] = {buf, bytes_to_read};
    bytes_remaining -= bytes_to_read;
  }

  auto mf = std::make_unique<my_file>();
  mf->fp = fp;
  mf->blocks = blocks;
  mf->current_block = 0;
  mf->current_offset = 0;
  mf->isfirst = 0;
  mf->s = std::move(s);
  mf->fi = std::move(fi);

  /* Add our submission queue entry to the tail of the SQE ring buffer */
  next_tail = tail = *sring->tail;
  next_tail++;
  read_barrier();
  index = tail & *s->sq_ring.ring_mask;
  struct io_uring_sqe *sqe = &s->sqes[index];
  sqe->fd = fd;
  sqe->flags = 0;
  sqe->opcode = IORING_OP_READV;
  sqe->addr = reinterpret_cast<unsigned long>(mf->fi->iovecs.data());
  sqe->len = blocks;
  sqe->off = 0;
  sqe->user_data = reinterpret_cast<unsigned long long>(mf->fi.get());
  sring->array[index] = index;
  tail = next_tail;

  if (*sring->tail != tail) {
    *sring->tail = tail;
    write_barrier();
  }

  if ((*sring->flags) & IORING_SQ_NEED_WAKEUP) {
    systemTimes++;
    int ret = io_uring_enter(mf->s->ring_fd, 1, 1, IORING_ENTER_SQ_WAKEUP);
    if (ret < 0) {
      perror("io_uring_enter");
      return nullptr;
    }
  }

  return mf;
}

size_t my_fread(void *ptr, size_t size, size_t count, my_file *mf) {
  struct submitter *s = mf->s.get();
  struct file_info *fi_read;
  struct app_io_cq_ring *cring = &s->cq_ring;
  struct io_uring_cqe *cqe;
  size_t total_bytes = size * count;
  size_t bytes_read = 0;

  unsigned head = *cring->head;
  unsigned tail = *cring->tail;
  unsigned index = mf->current_block;
  size_t offset = mf->current_offset;

  if (index >= mf->blocks)
    return 0;

  if (mf->isfirst == 0) {
    while (head == *cring->tail) {
      // usleep(1);
    }
  }
  do {
    read_barrier();

    if (mf->isfirst == 0 && head == *cring->tail)
      break;

    /* Get the entry */
    // If this is the first visit to this file's io_uring cqe queue, we need to
    // save it to reduce time spent.
    if (mf->isfirst == 0) {
      cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
      fi_read = reinterpret_cast<struct file_info *>(cqe->user_data);
      mf->fi_read.reset(fi_read);
      mf->isfirst = 1;
      if (cqe->res < 0) {
        std::cerr << "Error: " << strerror(abs(cqe->res)) << "\n";
        break;
      }
    } else {
      fi_read = mf->fi_read.get();
    }

    size_t block_size = BLOCK_SZ - offset;
    size_t to_copy = fi_read->iovecs[index].iov_len;
    size_t remaining_space = total_bytes - bytes_read;

    if (to_copy > block_size) {
      to_copy = block_size; // Only read to the end of the current block
    }

    if (to_copy > remaining_space) {
      to_copy = remaining_space; // Prevent buffer overflow
    }

    // Copy data from the current block into the user's buffer
    memcpy(static_cast<char*>(ptr) + bytes_read,
           static_cast<char*>(fi_read->iovecs[index].iov_base) + offset, to_copy);
    bytes_read += to_copy;
    offset += to_copy;

    // Move to the next block if we have finished the current one
    if (offset >= BLOCK_SZ) {
      index++;
      offset = 0; // Reset offset for the new block
      head++;     // Move the head forward to mark this CQE as seen
    }

    if (bytes_read >= total_bytes) {
      break; // We've read enough data
    }
  } while (index < mf->blocks);

  mf->current_block = index;
  mf->current_offset = offset;

  *cring->head = head;
  write_barrier();
  return bytes_read;
}

void cat(const char *filename) {
  auto mf = my_fopen(filename, "r");
  if (!mf) {
    perror("Failed to open file.");
  }

  size_t bytesRead;
  char buffer[1024];
  clock_t start = clock();
  while ((bytesRead = my_fread(buffer, sizeof(char), sizeof(buffer), mf.get())) > 0) {
    // TODO: shared buffer
    // write(STDOUT_FILENO, buffer, bytesRead);
  }
  clock_t end = clock();
  double cpu_time_used = static_cast<double>(end - start) / CLOCKS_PER_SEC;

  std::cout << "my_fread takes time: " << cpu_time_used << " seconds\n";
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <filename>\n";
    return 1;
  }
  clock_t start = clock();

  systemTimes = 0;
  for (int i = 1; i < argc; i++) {
    cat(argv[i]);
  }
  clock_t end = clock();
  double cpu_time_used = static_cast<double>(end - start) / CLOCKS_PER_SEC;

  std::cout << "Total time: " << cpu_time_used << " seconds\n";
  std::cout << "Use io_uring_enter system call times = " << systemTimes << "\n";
  return 0;
}
