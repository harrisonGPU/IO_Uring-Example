#define QUEUE_DEPTH 256
#define BLOCK_SZ 4096

/*
 * Filled with the offset for mmap(2)
 */
struct io_sqring_offsets_own {
  unsigned int head;
  unsigned int tail;
  unsigned int ring_mask;
  unsigned int ring_entries;
  unsigned int flags;
  unsigned int dropped;
  unsigned int array;
  unsigned int resv1;
  unsigned long long resv2;
};

struct io_cqring_offsets_own {
  unsigned int head;
  unsigned int tail;
  unsigned int ring_mask;
  unsigned int ring_entries;
  unsigned int overflow;
  unsigned int cqes;
  unsigned int flags;
  unsigned int resv1;
  unsigned long long resv2;
};

/*
 * Passed in for io_uring_setup(2). Copied back with updated info on success
 */
struct io_uring_params_own {
  unsigned int sq_entries;
  unsigned int cq_entries;
  unsigned int flags;
  unsigned int sq_thread_cpu;
  unsigned int sq_thread_idle;
  unsigned int features;
  unsigned int wq_fd;
  unsigned int resv[3];
  struct io_sqring_offsets_own sq_off;
  struct io_cqring_offsets_own cq_off;
};
/*
 * IO submission data structure (Submission Queue Entry)
 */
struct io_uring_sqe_own {
  unsigned char opcode;  /* type of operation for this sqe */
  unsigned char flags;   /* IOSQE_ flags */
  unsigned short ioprio; /* ioprio for the request */
  __signed__ int fd;     /* file descriptor to do IO on */
  union {
    unsigned long long off; /* offset into file */
    unsigned long long addr2;
  };
  union {
    unsigned long long addr; /* pointer to buffer or iovecs */
    unsigned long long splice_off_in;
  };
  unsigned int len; /* buffer size or number of iovecs */
  union {
    unsigned int rw_flags;
    unsigned int fsync_flags;
    unsigned short poll_events; /* compatibility */
    unsigned int poll32_events; /* word-reversed for BE */
    unsigned int sync_range_flags;
    unsigned int msg_flags;
    unsigned int timeout_flags;
    unsigned int accept_flags;
    unsigned int cancel_flags;
    unsigned int open_flags;
    unsigned int statx_flags;
    unsigned int fadvise_advice;
    unsigned int splice_flags;
    unsigned int rename_flags;
    unsigned int unlink_flags;
    unsigned int hardlink_flags;
  };
  unsigned long long user_data; /* data to be passed back at completion time */
  /* pack this to avoid bogus arm OABI complaints */
  union {
    /* index into fixed buffers, if used */
    unsigned short buf_index;
    /* for grouped buffer selection */
    unsigned short buf_group;
  } __attribute__((packed));
  /* personality to use, if used */
  unsigned short personality;
  union {
    __signed__ int splice_fd_in;
    unsigned int file_index;
  };
  unsigned long long __pad2[2];
};
struct io_uring_cqe_own {
  unsigned long long user_data; /* sqe->data submission passed back */
  __signed__ int res;           /* result code for this event */
  unsigned int flags;
};

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
  struct io_uring_cqe_own *cqes;
};

struct submitter {
  int ring_fd;
  app_io_sq_ring sq_ring;
  struct io_uring_sqe_own *sqes;
  app_io_cq_ring cq_ring;
};

struct iovc {
  void *buffer;              // Pointer to data
  unsigned long buffer_size; // Length of data
};

struct file_info {
  long long file_sz;
  struct iovc iovecs[]; /* Referred by readv/writev */
};

struct my_file {
  int fd;
  int blocks;
  int current_block;
  int isfirst;
  unsigned long current_offset;
  submitter *s;
  file_info *fi_read;
  file_info *fi;
};

int systemTimes = 0;
inline void read_barrier() { __atomic_thread_fence(__ATOMIC_ACQUIRE); }

inline void write_barrier() { __atomic_thread_fence(__ATOMIC_RELEASE); }

unsigned long my_fread(void *ptr, unsigned long size, unsigned long count,
                       my_file *mf) {
  struct submitter *s = mf->s;
  struct file_info *fi_read;
  struct app_io_cq_ring *cring = &s->cq_ring;
  struct io_uring_cqe_own *cqe;
  unsigned long total_bytes = size * count;
  unsigned long bytes_read = 0;

  unsigned head = *cring->head;
  unsigned tail = *cring->tail;
  unsigned index = mf->current_block;
  unsigned long offset = mf->current_offset;

  if (index >= mf->blocks)
    return 0;

  do {
    read_barrier();

    if (mf->isfirst == 0 && head == *cring->tail)
      break;

    /* Get the entry */
    // If this is the first visit to this file's io_uring cqe queue, we need to
    // save it to reduce time spent.
    if (mf->isfirst == 0) {
      cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
      fi_read = (struct file_info *)cqe->user_data;
      mf->fi_read = fi_read;
      mf->isfirst = 1;
      if (cqe->res < 0) {
        break;
      }
    } else {
      fi_read = mf->fi_read;
    }

    unsigned long block_size = BLOCK_SZ - offset;
    unsigned long to_copy = fi_read->iovecs[index].buffer_size;
    unsigned long remaining_space = total_bytes - bytes_read;

    if (to_copy > block_size) {
      to_copy = block_size; // Only read to the end of the current block
    }

    if (to_copy > remaining_space) {
      to_copy = remaining_space; // Prevent buffer overflow
    }

    // Copy data from the current block into the user's buffer
    __builtin_memcpy((char *)ptr + bytes_read,
                     (char *)fi_read->iovecs[index].buffer + offset, to_copy);
    bytes_read += to_copy;
    offset += to_copy;

    // Move to the next block if we have finished the current one
    if (offset >= BLOCK_SZ) {
      index++;
      offset = 0; // Reset offset for the new block
      __atomic_fetch_add(&head, 1, __ATOMIC_RELAXED);     // Move the head forward to mark this CQE as seen
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