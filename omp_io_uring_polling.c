#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <fcntl.h>

#define QUEUE_DEPTH 8
#define BUFFER_SIZE 1024

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
    int sring_sz;
    int cring_sz;
};

int io_uring_setup(unsigned entries, struct io_uring_params *params) {
    return (int) syscall(__NR_io_uring_setup, entries, params);
}

int app_setup_uring(struct submitter *s) {
    struct app_io_sq_ring *sring = &s->sq_ring;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_params params;
    void *sq_ptr, *cq_ptr;

    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;  // Ensure SQ Polling mode is set
    params.sq_thread_idle = 2000;        // Set idle time for polling thread

    s->ring_fd = io_uring_setup(QUEUE_DEPTH, &params);
    if (s->ring_fd < 0) {
        perror("io_uring_setup");
        return 1;
    }

    s->sring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    s->cring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        if (s->cring_sz > s->sring_sz) {
            s->sring_sz = s->cring_sz;
        }
        s->cring_sz = s->sring_sz;
    }

    sq_ptr = mmap(0, s->sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ptr = sq_ptr;
    } else {
        cq_ptr = mmap(0, s->cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
    }

    sring->head = sq_ptr + params.sq_off.head;
    sring->tail = sq_ptr + params.sq_off.tail;
    sring->ring_mask = sq_ptr + params.sq_off.ring_mask;
    sring->ring_entries = sq_ptr + params.sq_off.ring_entries;
    sring->flags = sq_ptr + params.sq_off.flags;
    sring->array = sq_ptr + params.sq_off.array;

    s->sqes = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_SQES);
    if (s->sqes == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    cring->head = cq_ptr + params.cq_off.head;
    cring->tail = cq_ptr + params.cq_off.tail;
    cring->ring_mask = cq_ptr + params.cq_off.ring_mask;
    cring->ring_entries = cq_ptr + params.cq_off.ring_entries;
    cring->cqes = cq_ptr + params.cq_off.cqes;

    // Print debug information
    printf("io_uring setup completed\n");

    return 0;
}

void io_uring_prep_read(struct io_uring_sqe *sqe, int fd, void *buf, unsigned nbytes, off_t offset) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (unsigned long) buf;
    sqe->len = nbytes;
    sqe->off = offset;
}

int handle_io_uring(struct submitter *s, int fd) {
    struct io_uring_cqe *cqe;
    struct io_uring_sqe *sqe;
    char buffer[BUFFER_SIZE];

    // Get an SQE (Submission Queue Entry) for reading the file
    sqe = &s->sqes[*s->sq_ring.tail & *s->sq_ring.ring_mask];
    io_uring_prep_read(sqe, fd, buffer, BUFFER_SIZE, 0);

    // Update the tail and array
    unsigned tail = *s->sq_ring.tail;
    unsigned index = tail & *s->sq_ring.ring_mask;
    s->sq_ring.array[index] = tail;

    *s->sq_ring.tail = tail + 1;

    // Print debug information
    printf("Submitted read request: fd=%d, tail=%u, index=%u\n", fd, tail, index);

    // No need to explicitly call io_uring_enter, the kernel thread will automatically handle the requests in the submission queue

    // Wait for completion
    while (*s->cq_ring.head == *s->cq_ring.tail) {
        usleep(1000);
    }

    // Get the CQE (Completion Queue Entry)
    cqe = &s->cq_ring.cqes[*s->cq_ring.head & *s->cq_ring.ring_mask];

    // Check the result
    if (cqe->res < 0) {
        fprintf(stderr, "Read failed: %d\n", cqe->res);
        return 1;
    }

    // Output the read string
    buffer[cqe->res] = '\0'; // Ensure the string is null-terminated
    printf("Read result: %s\n", buffer);

    // Mark the CQE as seen
    *s->cq_ring.head += 1;

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file1> <file2> ... <fileN>\n", argv[0]);
        return 1;
    }

    struct submitter *s = (struct submitter *)malloc(sizeof(struct submitter));
    if (s == NULL) {
        fprintf(stderr, "Failed to allocate memory for submitter.\n");
        return 1;
    }
    memset(s, 0, sizeof(struct submitter));

    // Setup io_uring with SQ Polling
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000; // 2 seconds idle time

    s->ring_fd = io_uring_setup(QUEUE_DEPTH, &params);
    if (s->ring_fd < 0) {
        perror("io_uring_setup");
        free(s);
        return 1;
    }

    if (app_setup_uring(s)) {
        fprintf(stderr, "Failed to setup io_uring.\n");
        free(s);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            perror("open");
            continue;
        }

        if (handle_io_uring(s, fd)) {
            fprintf(stderr, "Failed to handle io_uring for file: %s\n", argv[i]);
        }

        close(fd);
    }

    // Cleanup
    close(s->ring_fd);
    munmap(s->sq_ring.head, s->sring_sz);
    munmap(s->cq_ring.head, s->cring_sz);
    free(s);

    return 0;
}
