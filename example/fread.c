#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  int fd;
  size_t pos;
} MY_FILE;

double perFileTime;

off_t get_file_size(int fd) {
  struct stat st;

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

MY_FILE *my_fopen(const char *filename, const char *mode) {
  MY_FILE *file = (MY_FILE *)malloc(sizeof(MY_FILE));
  if (!file) {
    perror("Failed to allocate memory for MY_FILE");
    return NULL;
  }

  int flags = O_RDONLY;
  file->fd = open(filename, flags);
  if (file->fd == -1) {
    perror("Failed to open file");
    free(file);
    return NULL;
  }

  file->pos = 0;
  return file;
}

size_t my_fread(void *ptr, size_t size, size_t count, MY_FILE *stream) {
  if (stream == NULL || ptr == NULL) {
    errno = EINVAL;
    return 0;
  }

  size_t total_bytes = size * count;
  ssize_t bytes_read = read(stream->fd, ptr, total_bytes);
  if (bytes_read == -1) {
    perror("Failed to read file");
    return 0;
  }

  stream->pos += bytes_read;
  return bytes_read / size;
}

int my_fclose(MY_FILE *stream) {
  if (stream == NULL) {
    return EOF;
  }

  close(stream->fd);
  free(stream);
  return 0;
}

void my_cat(const char *filename) {
  MY_FILE *file = my_fopen(filename, "r");
  if (!file) {
    perror("Failed to open file with my_fopen");
    return;
  }

  char buffer[1024];
  size_t bytes_read;
  clock_t start = clock();
  while ((bytes_read = my_fread(buffer, 1, sizeof(buffer), file)) > 0) {
    write(STDOUT_FILENO, buffer, bytes_read);
  }
  clock_t end = clock();
  double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
  perFileTime += cpu_time_used;
  printf(
      "Completed reading '%s': Duration = %f seconds, File Size = %ld bytes.\n",
      filename, cpu_time_used, get_file_size(file->fd));
  my_fclose(file);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    return 1;
  }
  perFileTime = 0;
  clock_t start = clock();
  for (int i = 1; i < argc; i++) {
    my_cat(argv[i]);
  }
  clock_t end = clock();
  double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;

  printf("Total time to read %d files: %f seconds\n", argc - 1, cpu_time_used);
  printf("Average time per read file: %f seconds\n", perFileTime / (argc - 1));
  return 0;
}
