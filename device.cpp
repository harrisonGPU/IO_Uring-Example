#include "my_io.h"
#include <cstring>

int systemTimes = 0;

size_t my_fread(void *ptr, size_t size, size_t count, my_file *mf) {
  struct submitter *s = mf->s;
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
      fi_read = (struct file_info *)cqe->user_data;
      mf->fi_read = fi_read;
      mf->isfirst = 1;
      if (cqe->res < 0) {
        break;
      }
    } else {
      fi_read = mf->fi_read;
    }

    size_t block_size = BLOCK_SZ - offset;
    size_t to_copy = fi_read->iovecs[index].buffer_size;
    size_t remaining_space = total_bytes - bytes_read;

    if (to_copy > block_size) {
      to_copy = block_size; // Only read to the end of the current block
    }

    if (to_copy > remaining_space) {
      to_copy = remaining_space; // Prevent buffer overflow
    }

    // Copy data from the current block into the user's buffer
    memcpy((char *)ptr + bytes_read,
           (char *)fi_read->iovecs[index].buffer + offset, to_copy);
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