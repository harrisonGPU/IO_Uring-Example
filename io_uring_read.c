#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>  // For PRIdMAX
#include <sys/types.h> // For off_t

/* If your compilation fails because the header file below is missing,
 * your kernel is probably too old to support io_uring.
 * */
#include <linux/io_uring.h>

#define QUEUE_DEPTH 128
#define BLOCK_SZ    4096
int first;
/* This is x86 specific */
#define read_barrier()  __asm__ __volatile__("":::"memory")
#define write_barrier() __asm__ __volatile__("":::"memory")

/* Macros for barriers needed by io_uring */
#define io_uring_smp_store_release(p, v)            \
    atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), \
                  memory_order_release)
#define io_uring_smp_load_acquire(p)                \
    atomic_load_explicit((_Atomic typeof(*(p)) *)(p),   \
                 memory_order_acquire)
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
    struct iovec iovecs[];      /* Referred by readv/writev */
};

typedef struct {
  FILE *fp;
  struct submitter *s;
  struct file_info *fi;
} my_file;


/*
 * This code is written in the days when io_uring-related system calls are not
 * part of standard C libraries. So, we roll our own system call wrapper
 * functions.
 * */

int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int) syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
                          unsigned int min_complete, unsigned int flags)
{
    return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                   flags, NULL, 0);
}

int io_uring_register(unsigned int fd, unsigned int opcode,
					  const void *arg, unsigned int nr_args)
{
	return (int) syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}


off_t get_file_size(int fd) {
    struct stat st;

    if(fstat(fd, &st) < 0) {
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
    } else if (S_ISREG(st.st_mode))
        return st.st_size;

    return -1;
}

off_t get_file_size2(FILE *file) {
    struct stat st;
    int fd = fileno(file);  // 获取与 FILE* 关联的文件描述符

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


int app_setup_uring(struct submitter *s) {
    memset(s, 0, sizeof(*s));
    struct app_io_sq_ring *sring = &s->sq_ring;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_params p;
    void *sq_ptr, *cq_ptr;

    memset(&p, 0, sizeof(p));
    p.flags |= IORING_SETUP_SQPOLL;
    p.flags |= IORING_SETUP_SQ_AFF;
    p.sq_thread_idle = 200000;
    //p.sq_thread_cpu = 4;
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

    sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE, 
            MAP_SHARED | MAP_POPULATE,
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
                MAP_SHARED | MAP_POPULATE,
                s->ring_fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
    }

    sring->head = sq_ptr + p.sq_off.head;
    sring->tail = sq_ptr + p.sq_off.tail;
    sring->ring_mask = sq_ptr + p.sq_off.ring_mask;
    sring->ring_entries = sq_ptr + p.sq_off.ring_entries;
    sring->flags = sq_ptr + p.sq_off.flags;
    sring->array = sq_ptr + p.sq_off.array;

    s->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            s->ring_fd, IORING_OFF_SQES);
    if (s->sqes == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    cring->head = cq_ptr + p.cq_off.head;
    cring->tail = cq_ptr + p.cq_off.tail;
    cring->ring_mask = cq_ptr + p.cq_off.ring_mask;
    cring->ring_entries = cq_ptr + p.cq_off.ring_entries;
    cring->cqes = cq_ptr + p.cq_off.cqes;

    return 0;
}

void output_to_console(char *buf, int len) {
    while (len--) {
        fputc(*buf++, stdout);
    }
}


void my_fread(size_t size, size_t count, my_file *restrict mf) {
    struct submitter*s = mf->s;
    struct file_info *fi;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_cqe *cqe;
    size_t total_bytes = size * count;
    unsigned head, reaped = 0;

    head = *cring->head;

    while (head == *cring->tail) {
        usleep(100);
    }
    do {
        read_barrier();

        if (head == *cring->tail)
            break;

        /* Get the entry */
        cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
        fi = (struct file_info*) cqe->user_data;
        if (cqe->res < 0)
            fprintf(stderr, "Error: %s\n", strerror(abs(cqe->res)));

        int blocks = (int) total_bytes / BLOCK_SZ;
        if (total_bytes % BLOCK_SZ) blocks++;

        for (int i = 0; i < blocks; i++)
            output_to_console(fi->iovecs[i].iov_base, fi->iovecs[i].iov_len);

        head++;
    } while (1);

    *cring->head = head;
    write_barrier();
}

my_file *my_fopen(const char *filename, const char *mode) {
    struct submitter *s = malloc(sizeof(struct submitter));
    struct file_info *fi;
    if(app_setup_uring(s)) {
        fprintf(stderr, "Unable to setup uring!\n");
        free(s);
        return NULL;
    }

    // TODO: Write file
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

    //printf("fd is %d, fd1 is %d\n", fd, fd1);
    struct app_io_sq_ring *sring = &s->sq_ring;
    unsigned index = 0, current_block = 0, tail = 0, next_tail = 0;

    off_t file_sz = get_file_size2(fp);
    printf("The size of the file is: %" PRIdMAX " bytes\n", (intmax_t)file_sz);
    if (file_sz < 0)
      return NULL;
    off_t bytes_remaining = file_sz;
    int blocks = (int)file_sz / BLOCK_SZ;
    if (file_sz % BLOCK_SZ)
      blocks++;
    
    fi = malloc(sizeof(*fi) + sizeof(struct iovec) * blocks);
    if (!fi) {
         fprintf(stderr, "Unable to allocate memory\n");
         return NULL;
    }
    fi->file_sz = file_sz;
    my_file *mf = malloc(sizeof(my_file));;
    mf->s = s;
    mf->fi = fi;
    mf->fp = fp;

    while (bytes_remaining) {
      off_t bytes_to_read = bytes_remaining;
      if (bytes_to_read > BLOCK_SZ)
        bytes_to_read = BLOCK_SZ;

      fi->iovecs[current_block].iov_len = bytes_to_read;

      void *buf;
      if (posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ)) {
        perror("posix_memalign");
        return NULL;
      }
      fi->iovecs[current_block].iov_base = buf;

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
      first++;
      int ret = io_uring_enter(s->ring_fd, 1, 1, IORING_ENTER_SQ_WAKEUP);
      if (ret < 0) {
        perror("io_uring_enter");
        return NULL;
      }
    }
    return mf;
}

int main(int argc, char *argv[]) {
    first = 0;
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }
    clock_t start = clock();

    for (int i = 1; i < argc; i++)
    {
        my_file *mf = my_fopen(argv[i], "r");
        if (mf != NULL) {
            my_fread(mf->fi->file_sz, 1,mf);
        }
        else {
            printf("Fopen Fail!");
            break;
        }
    }
    clock_t end = clock();
    double cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
    printf("Elapsed time: %f seconds\n", cpu_time_used);

    printf("Use io_uring_enter system call times = %d\n", first);
    return 0;
}
