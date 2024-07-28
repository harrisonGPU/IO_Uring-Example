#include "my_io.h"
#include <iostream>
#include <fcntl.h>
#include <linux/fs.h>
#include <memory>
#include <stdatomic.h>
#include <stddef.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <omp.h>

int io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return (int)syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags) {
  return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                      flags, NULL, 0);
}

int io_uring_register(unsigned int fd, unsigned int opcode, const void *arg,
                      unsigned int nr_args) {
  return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
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

void update_file_size(my_file *mf) {
    if (mf && mf->fp) {
        fseek(mf->fp, 0, SEEK_END);
        mf->fi->file_sz = ftell(mf->fp);
        fseek(mf->fp, 0, SEEK_SET);
    }
}

my_file *my_fopen(const char *filename, const char *mode) {
  struct submitter *s = static_cast<struct submitter*>(omp_alloc(sizeof(struct submitter), llvm_omp_target_shared_mem_alloc));
  struct file_info *fi;
  if (app_setup_uring(s)) {
    fprintf(stderr, "Unable to setup uring!\n");
    free(s);
    return NULL;
  }

  FILE *fp = fopen(filename, mode);
  if (!fp) {
    printf("Fopen Failed!");
    return NULL;
  }
  int fd = fileno(fp);

  if (fd < 0) {
    printf("Fopen fileno");
    return NULL;
  }

  struct app_io_sq_ring *sring = &s->sq_ring;
  unsigned index = 0, current_block = 0, tail = 0, next_tail = 0;

  off_t file_sz = get_file_size(fp);
  if (file_sz < 0)
    return NULL;
  off_t bytes_remaining = file_sz;
  int blocks = (int)file_sz / BLOCK_SZ;
  if (file_sz % BLOCK_SZ)
    blocks++;

  int result = io_uring_register(s->ring_fd, IORING_REGISTER_FILES, &fd, 1);
  if (result < 0) {
    perror("io_uring_register_fds failed");
    fclose(fp);
    omp_free(s, llvm_omp_target_shared_mem_alloc);
    return NULL;
  }
  
  fi = static_cast<struct file_info *>(omp_alloc(sizeof(*fi) + sizeof(struct iovec) * blocks, llvm_omp_target_shared_mem_alloc));
  if (!fi) {
    fprintf(stderr, "Unable to allocate memory\n");
    return NULL;
  }
  fi->file_sz = file_sz;
  my_file *mf = static_cast<my_file *>(omp_alloc(sizeof(my_file), llvm_omp_target_shared_mem_alloc));
  mf->s = s;
  mf->fi = fi;
  mf->fp = fp;
  mf->blocks = blocks;
  mf->current_block = 0;
  mf->current_offset = 0;
  mf->isfirst = 0;

  while (bytes_remaining) {
    off_t bytes_to_read = bytes_remaining;
    if (bytes_to_read > BLOCK_SZ)
      bytes_to_read = BLOCK_SZ;

    fi->iovecs[current_block].buffer_size = bytes_to_read;

    void *buf;
    if (posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ)) {
      perror("posix_memalign");
      return NULL;
    }
    fi->iovecs[current_block].buffer = buf;

    current_block++;
    bytes_remaining -= bytes_to_read;
  }

  /* Add our submission queue entry to the tail of the SQE ring buffer */
  next_tail = tail = *sring->tail;
  next_tail++;
  read_barrier();
  index = tail & *s->sq_ring.ring_mask;
  struct io_uring_sqe *sqe = &s->sqes[index];
  sqe->fd = fd;
  sqe->flags = 0;
  sqe->opcode = IORING_OP_READV;
  sqe->addr = (unsigned long)fi->iovecs;
  sqe->len = blocks;
  sqe->off = 0;
  sqe->user_data = (unsigned long long)fi;
  sring->array[index] = index;
  tail = next_tail;

  if (*sring->tail != tail) {
    *sring->tail = tail;
    write_barrier();
  }

  if ((*sring->flags) & IORING_SQ_NEED_WAKEUP) {
    systemTimes++;
    int ret = io_uring_enter(s->ring_fd, 1, 1, IORING_ENTER_SQ_WAKEUP);
    if (ret < 0) {
      perror("io_uring_enter");
      return NULL;
    }
  }

  return mf;
}

void my_fclose(my_file *mf) {
    if (mf == NULL) {
        return; // If the pointer is NULL, no deallocation is needed.
    }

    // Close the file descriptor if open.
    if (mf->fp) {
        fclose(mf->fp);
        mf->fp = NULL;
    }

    // Free the file_info structure.
    if (mf->fi) {
        omp_free(mf->fi, llvm_omp_target_shared_mem_alloc);
        mf->fi = NULL;
    }

    // Free the submitter structure and any associated resources.
    if (mf->s) {
        // Continue similarly for other mmap'ed or allocated regions within submitter if any.
        omp_free(mf->s, llvm_omp_target_shared_mem_alloc);
        mf->s = NULL;
    }

    // Finally, free the my_file structure itself.
    omp_free(mf,llvm_omp_target_shared_mem_alloc);

    return;
}

int app_setup_uring(struct submitter *s) {
  memset(s, 0, sizeof(*s));
  struct app_io_sq_ring *sring = &s->sq_ring;
  struct app_io_cq_ring *cring = &s->cq_ring;
  struct io_uring_params p;
  void *sq_ptr, *cq_ptr;

  memset(&p, 0, sizeof(p));
  p.flags |= IORING_SETUP_SQPOLL;
  p.flags |= IORING_SETUP_SQ_AFF;
  p.sq_thread_idle = 20000000;
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
    cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) {
      perror("mmap");
      return 1;
    }
  }

  sring->head = (unsigned *)((char *)sq_ptr + p.sq_off.head);
  sring->tail = (unsigned *)((char *)sq_ptr + p.sq_off.tail);
  sring->ring_mask = (unsigned *)((char *)sq_ptr + p.sq_off.ring_mask);
  sring->ring_entries = (unsigned *)((char *)sq_ptr + p.sq_off.ring_entries);
  sring->flags = (unsigned *)((char *)sq_ptr + p.sq_off.flags);
  sring->array = (unsigned *)((char *)sq_ptr + p.sq_off.array);

  s->sqes = (struct io_uring_sqe *)mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd,
                 IORING_OFF_SQES);
  if (s->sqes == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  cring->head = (unsigned *)((char *)cq_ptr + p.cq_off.head);
  cring->tail = (unsigned *)((char *)cq_ptr + p.cq_off.tail);
  cring->ring_mask = (unsigned *)((char *)cq_ptr + p.cq_off.ring_mask);
  cring->ring_entries = (unsigned *)((char *)cq_ptr + p.cq_off.ring_entries);
  cring->cqes = (struct io_uring_cqe *)((char *)cq_ptr + p.cq_off.cqes);

  return 0;
}

