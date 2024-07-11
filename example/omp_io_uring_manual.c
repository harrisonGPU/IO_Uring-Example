#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <linux/io_uring.h>

#define QUEUE_DEPTH 1
#define BLOCK_SZ    1024
#define FILE_SIZE_TYPE off_t

/* This is x86 specific */
#define read_barrier()  __asm__ __volatile__("":::"memory")
#define write_barrier() __asm__ __volatile__("":::"memory")

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
    FILE_SIZE_TYPE file_sz;
    struct iovec iovecs[];      /* Referred by readv/writev */
};

/*
 * This code is written in the days when io_uring-related system calls are not
 * part of standard C libraries. So, we roll our own system call wrapper
 * functions.
 */

int io_uring_setup(unsigned entries, struct io_uring_params *params)
{
    return (int) syscall(__NR_io_uring_setup, entries, params);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags) {
  return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                      flags, NULL, 0);
}

/* 
 * Returns the file size by using the file descriptor provided. It handles both
 * regular files and block devices. 
 */
FILE_SIZE_TYPE get_file_size(int file_descriptor) {
    struct stat file_stats;

    if(fstat(file_descriptor, &file_stats) < 0) {
        perror("fstat");
        return -1;
    }
    if (S_ISBLK(file_stats.st_mode)) {
        unsigned long long size_in_bytes;
        if (ioctl(file_descriptor, BLKGETSIZE64, &size_in_bytes) != 0) {
            perror("ioctl");
            return -1;
        }
        return size_in_bytes;
    } else if (S_ISREG(file_stats.st_mode)) {
        return file_stats.st_size;
    }
    return -1;
}

/*
 * Setting up io_uring might initially appear complex due to the extensive 
 * configuration required. However, the complexity is manageable once 
 * understood. To simplify interaction with io_uring, liburing was developed
 * offering an easier interface. It is beneficial to invest time in 
 * understanding these underlying operations—not only does this deepen
 * one's knowledge but also enhances debugging capabilities. Grasping 
 * the inner workings of io_uring not only provides technical prowess but 
 * also delivers a unique satisfaction from mastering such an intricate 
 * component of modern Linux I/O.
 */


int app_setup_uring(struct submitter *s) {
    struct app_io_sq_ring *sring = &s->sq_ring;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_params params;
    void *sq_ptr, *cq_ptr;

    /*
     * The io_uring_setup() function requires the io_uring_params structure to
     * be initialized to zero before passing. While it is possible to set
     * various flags within this structure to modify behavior, no flags are set
     * in this particular example.
     */

    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL; 
    params.sq_thread_idle = 1000; 

    s->ring_fd = io_uring_setup(QUEUE_DEPTH, &params);
    if (s->ring_fd < 0) {
        perror("io_uring_setup");
        return 1;
    }

    /*
     * io_uring facilitates communication through two shared ring buffers
     * between the kernel and user space, allowing for efficient data
     * management. In newer kernel versions, these two buffers—the submission
     * queue and the completion queue—can be mapped simultaneously with a single
     * mmap() call. The submission queue includes an additional indirection
     * layer for managing I/O operations, which is also integrated into the
     * mapped space.
     */

    int sring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    int cring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);

    /*
     * Starting with kernel version 5.4, it is possible to map both submission
     * and completion buffers using a single mmap() call. Instead of checking
     * for specific kernel versions, a more robust approach involves inspecting
     * the 'features' field in the io_uring_params structure. This field is a
     * bitmask. If the IORING_FEAT_SINGLE_MMAP flag is set, only one mmap() call
     * is necessary to map both buffers, eliminating the need for a second call
     * to separately map the completion ring.
     */

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        if (cring_sz > sring_sz) {
            sring_sz = cring_sz;
        }
        cring_sz = sring_sz;
    }

    /* 
     * Map in the submission and completion queue ring buffers.
     * Older kernels only map in the submission queue, though.
     */
    sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE, 
            MAP_SHARED | MAP_POPULATE,
            s->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
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
    /* Save useful fields in a global app_io_sq_ring struct for later
     * easy reference */
    sring->head = sq_ptr + params.sq_off.head;
    sring->tail = sq_ptr + params.sq_off.tail;
    sring->ring_mask = sq_ptr + params.sq_off.ring_mask;
    sring->ring_entries = sq_ptr + params.sq_off.ring_entries;
    sring->flags = sq_ptr + params.sq_off.flags;
    sring->array = sq_ptr + params.sq_off.array;

    /* Map in the submission queue entries array */
    s->sqes = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            s->ring_fd, IORING_OFF_SQES);
    if (s->sqes == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Save useful fields in a global app_io_cq_ring struct for later
     * easy reference */
    cring->head = cq_ptr + params.cq_off.head;
    cring->tail = cq_ptr + params.cq_off.tail;
    cring->ring_mask = cq_ptr + params.cq_off.ring_mask;
    cring->ring_entries = cq_ptr + params.cq_off.ring_entries;
    cring->cqes = cq_ptr + params.cq_off.cqes;

    return 0;
}

void output_to_console(char *buf, int len) {
    fwrite(buf, len, 1, stdout);
}

/*
 * This function reads completion events from the completion queue. It retrieves
 * the data buffer containing file data and outputs it to the console.
 */


void read_from_cq(struct submitter *s) {
    struct file_info *fi;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_cqe *cqe;
    unsigned head, reaped = 0;

    head = *cring->head;
    printf("Reading from completion queue...\n");

    if (head == *cring->tail)
        printf("Ring buffer is empty.\n");
    do {
        read_barrier();
        /*
         * Remember, this is a ring buffer. If head == tail, it means that the
         * buffer is empty.
         */
        if (head == *cring->tail)
            break;

        /* Get the entry */
        cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
        fi = (struct file_info*) cqe->user_data;
        if (cqe->res < 0)
            fprintf(stderr, "Error: %s\n", strerror(abs(cqe->res)));
        else
            printf("Operation completed successfully, result: %d\n", cqe->res);
        
        int blocks = (int) fi->file_sz / BLOCK_SZ;
        if (fi->file_sz % BLOCK_SZ) blocks++;

        for (int i = 0; i < blocks; i++)
            output_to_console(fi->iovecs[i].iov_base, fi->iovecs[i].iov_len);

        head++;
    } while (1);

    *cring->head = head;
    write_barrier();
}

int submit_write_request(const char *file_path, const char *data, size_t data_size, struct submitter *s) {
    int file_fd = open(file_path, O_WRONLY | O_CREAT, 0644);
    if (file_fd < 0) {
        perror("open");
        return 1;
    }

    struct app_io_sq_ring *sring = &s->sq_ring;
    unsigned tail = *sring->tail;
    unsigned index = tail & *sring->ring_mask;
    struct io_uring_sqe *sqe = &s->sqes[index];

    sqe->fd = file_fd;
    sqe->opcode = IORING_OP_WRITEV;
    sqe->flags = 0;
    sqe->off = 0;
    sqe->addr = (unsigned long) &((struct iovec){.iov_base = (void *)data, .iov_len = data_size});
    sqe->len = 1; // Number of iovec structures
    sqe->user_data = (unsigned long long) file_fd;

    *sring->tail = tail + 1;
    write_barrier();

    // Notify the kernel and wait for at least one event to complete
    if (io_uring_enter(s->ring_fd, 1, 1, IORING_ENTER_GETEVENTS) < 0) {
        perror("io_uring_enter");
        close(file_fd);
        return 1;
    }

    close(file_fd);
    return 0;
}

/*
 * This function submits requests to the submission queue. The specific type of
 * request used here is readv(), designated by IORING_OP_READV, which allows for
 * reading data into multiple buffers.
 */
int submit_to_sq(char *file_path, struct submitter *s) {
    struct file_info *fi;

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0 ) {
        perror("Error opening file.");
        return 1;
    }

    printf("File opened successfully: %s\n", file_path);

    struct app_io_sq_ring *sring = &s->sq_ring;
    unsigned index = 0, current_block = 0, tail = 0, next_tail = 0;

    FILE_SIZE_TYPE file_sz = get_file_size(file_fd);
    if (file_sz < 0)
        return 1;
    FILE_SIZE_TYPE bytes_remaining = file_sz;
    int blocks = (int) file_sz / BLOCK_SZ;
    if (file_sz % BLOCK_SZ) blocks++;

    fi = malloc(sizeof(*fi) + sizeof(struct iovec) * blocks);
    if (!fi) {
        fprintf(stderr, "Unable to allocate memory\n");
        return 1;
    }
    fi->file_sz = file_sz;

    /*
     * For each block of the file we need to read, we allocate an iovec struct
     * which is indexed into the iovecs array. This array is passed in as part
     * of the submission. If you don't understand this, then you need to look
     * up how the readv() and writev() system calls work.
     * */
    while (bytes_remaining) {
        FILE_SIZE_TYPE bytes_to_read = bytes_remaining;
        if (bytes_to_read > BLOCK_SZ)
            bytes_to_read = BLOCK_SZ;

        fi->iovecs[current_block].iov_len = bytes_to_read;

        void *buf;
        if( posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ)) {
            perror("posix_memalign");
            return 1;
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
    sqe->fd = file_fd;
    sqe->flags = 0;
    sqe->opcode = IORING_OP_READV;
    sqe->addr = (unsigned long) fi->iovecs;
    sqe->len = blocks;
    sqe->off = 0;
    sqe->user_data = (unsigned long long) fi;
    sring->array[index] = index;
    tail = next_tail;

    /* Update the tail so the kernel can see it. */
    if(*sring->tail != tail) {
        *sring->tail = tail;
        write_barrier();
    }

    /*
     * Notifies the kernel of submitted events using the io_uring_enter() system
     * call. This function includes the IOURING_ENTER_GETEVENTS flag, which
     * instructs io_uring_enter() to block execution until a specified minimum
     * number of events (third parameter, min_complete) have been completed.
     */

    printf("Submitting read request for file: %s\n", file_path);
    int ret =  io_uring_enter(s->ring_fd, 1,0,
            IORING_ENTER_SQ_WAKEUP);
    if(ret < 0) {
        perror("io_uring_enter");
        return 1;
    }
    printf("io_uring_enter submitted successfully.\n");

    return 0;
}

int main(int argc, char *argv[]) {
    struct submitter *uring_submitter;
    int user_command = 0;

    // Ensure the correct command line usage
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    // Allocate memory for the io_uring submitter structure
    uring_submitter = malloc(sizeof(*uring_submitter));
    if (!uring_submitter) {
        perror("Failed to allocate memory for io_uring setup");
        return 1;
    }
    memset(uring_submitter, 0, sizeof(*uring_submitter));

    // Set up io_uring structures and verify setup success
    if (app_setup_uring(uring_submitter)) {
        fprintf(stderr, "Failed to setup io_uring.\n");
        return 1;
    }

    // Prompt user for operation type: write (1) or read (2)
    printf("Enter command (1 for write, 2 for read): ");
    fflush(stdout);
    scanf("%d", &user_command);

    if (user_command == 1) {
        const char *file_path = argv[1];
        const char *data_to_write = "Hello, GPU first";
        size_t data_length = strlen(data_to_write);

        // Submit a write request
        if (submit_write_request(file_path, data_to_write, data_length, uring_submitter)) {
            fprintf(stderr, "Error writing to file: %s\n", file_path);
            return 1;
        }

        printf("Data successfully written to %s.\n", file_path);
    } else if (user_command == 2) {
        // Loop through all provided files and read them
        for (int i = 1; i < argc; i++) {
            if (submit_to_sq(argv[i], uring_submitter)) {
                fprintf(stderr, "Error reading file: %s\n", argv[i]);
                return 1;
            }

            usleep(1000);
            read_from_cq(uring_submitter);
        }
    } else {
        fprintf(stderr, "Invalid command. Please enter 1 for write or 2 for read.\n");
        return 1;
    }

    return 0;
}