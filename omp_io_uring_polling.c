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

#define QUEUE_DEPTH 1
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

int io_uring_setup(unsigned entries, struct io_uring_params *params)
{
    return (int) syscall(__NR_io_uring_setup, entries, params);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                        flags, NULL, 0);
}

int app_setup_uring(struct submitter *s) {
    struct app_io_sq_ring *sring = &s->sq_ring;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_params params;
    void *sq_ptr, *cq_ptr;

    memset(&params, 0, sizeof(params));

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

    sq_ptr = mmap(0, s->sring_sz, PROT_READ | PROT_WRITE, 
                  MAP_SHARED | MAP_POPULATE,
                  s->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ptr = sq_ptr;
    } else {
        cq_ptr = mmap(0, s->cring_sz, PROT_READ | PROT_WRITE, 
                      MAP_SHARED | MAP_POPULATE,
                      s->ring_fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            perror("Mmap for completion queue");
            return 1;
        }
    }

    sring->head = sq_ptr + params.sq_off.head;
    sring->tail = sq_ptr + params.sq_off.tail;
    sring->ring_mask = sq_ptr + params.sq_off.ring_mask;
    sring->ring_entries = sq_ptr + params.sq_off.ring_entries;
    sring->flags = sq_ptr + params.sq_off.flags;
    sring->array = sq_ptr + params.sq_off.array;

    s->sqes = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   s->ring_fd, IORING_OFF_SQES);
    if (s->sqes == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    cring->head = cq_ptr + params.cq_off.head;
    cring->tail = cq_ptr + params.cq_off.tail;
    cring->ring_mask = cq_ptr + params.cq_off.ring_mask;
    cring->ring_entries = cq_ptr + params.cq_off.ring_entries;
    cring->cqes = cq_ptr + params.cq_off.cqes;

    return 0;
}

void io_uring_prep_poll_add(struct io_uring_sqe *sqe, int fd, short poll_mask) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll_events = poll_mask;
}

void io_uring_prep_read(struct io_uring_sqe *sqe, int fd, void *buf, unsigned nbytes, off_t offset) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (unsigned long) buf;
    sqe->len = nbytes;
    sqe->off = offset;
}

int main(int argc, char *argv[])
{
    struct submitter s;
    struct io_uring_cqe *cqe;
    struct io_uring_sqe *sqe;
    char buffer[BUFFER_SIZE];
    int ret, fd;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }

    // Open the file for reading
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // Setup io_uring
    if (app_setup_uring(&s)) {
        fprintf(stderr, "Failed to setup io_uring.\n");
        close(fd);
        return 1;
    }

    // Get an SQE (Submission Queue Entry) for polling
    sqe = &s.sqes[*s.sq_ring.tail & *s.sq_ring.ring_mask];
    io_uring_prep_poll_add(sqe, fd, POLLIN);

    // Update the tail for polling
    *s.sq_ring.tail += 1;
    // Submit the polling request
    printf("Submitting poll request...\n");
    ret = io_uring_enter(s.ring_fd, 1, 0, IORING_ENTER_GETEVENTS);
    if (ret < 0) {
        perror("Submit the polling request fail.");
        close(fd);
        return 1;
    }
    printf("Submitted poll request successful.\n");

    // Wait for the poll completion
    printf("Waiting for poll completion...\n");
    while (*s.cq_ring.head == *s.cq_ring.tail) {
        usleep(1000);
    }
    printf("Waiting for poll completion successful.\n");


    // Get the CQE (Completion Queue Entry) for polling
    cqe = &s.cq_ring.cqes[*s.cq_ring.head & *s.cq_ring.ring_mask];
    if (cqe->res < 0) {
        fprintf(stderr, "CQE for polling failed: %d\n", cqe->res);
        close(fd);
        return 1;
    }

    // Mark the CQE as seen for polling
    *s.cq_ring.head += 1;

    // Get an SQE (Submission Queue Entry) for reading the file
    sqe = &s.sqes[*s.sq_ring.tail & *s.sq_ring.ring_mask];
    io_uring_prep_read(sqe, fd, buffer, BUFFER_SIZE - 1, 0);

    // Update the tail
    *s.sq_ring.tail += 1;

    // Submit the request
    printf("Submitting read request...\n");
    ret = io_uring_enter(s.ring_fd, 1, 0, IORING_ENTER_GETEVENTS);
    if (ret < 0) {
        perror("io_uring_enter");
        close(fd);
        return 1;
    }
    printf("Read request submitted.\n");

    // Wait for completion
    printf("Waiting for completion...\n");
    while (*s.cq_ring.head == *s.cq_ring.tail) {
        usleep(1000);
    }
    printf("Waiting for completion successful.\n");
    // Get the CQE (Completion Queue Entry)
    cqe = &s.cq_ring.cqes[*s.cq_ring.head & *s.cq_ring.ring_mask];

    // Check the result
    if (cqe->res < 0) {
        fprintf(stderr, "Check the result read failed: %d\n", cqe->res);
        close(fd);
        return 1;
    }

    // Output the read string
    buffer[cqe->res] = '\0'; // Ensure the string is null-terminated
    printf("File content: %s\n", buffer);

    // Mark the CQE as seen
    *s.cq_ring.head += 1;

    // Cleanup
    close(fd);
    close(s.ring_fd);
    munmap(s.sq_ring.head, s.sring_sz);
    munmap(s.cq_ring.head, s.cring_sz);

    return 0;
}