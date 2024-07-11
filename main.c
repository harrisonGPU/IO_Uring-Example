#include "my_io.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>
double perFileTime;
void cat(const char *filename) {
  my_file *mf = my_fopen(filename, "r");
  if (!mf) {
    perror("Failed to open file.");
    return;
  }

  size_t bytesRead;
  char buffer[1024];
  clock_t start = clock();
  while ((bytesRead = my_fread(buffer, sizeof(char), sizeof(buffer), mf)) > 0) {
    // TODO: shared buffer
    // write(STDOUT_FILENO, buffer, bytesRead);
  }
  clock_t end = clock();
  double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
  perFileTime += cpu_time_used;
  //printf("Completed reading '%s': Duration = %f seconds, File Size = %ld bytes.\n", filename, cpu_time_used, mf->fi->file_sz);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    return 1;
  }
  perFileTime = 0;
  clock_t start = clock();

  systemTimes = 0;
  for (int i = 1; i < argc; i++) {
    cat(argv[i]);
  }
  clock_t end = clock();
  double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;

  printf("Total time to read %d files: %f seconds\n", argc - 1, cpu_time_used);
  printf("Average time per read file: %f seconds\n", perFileTime / (argc - 1));
  printf("Use io_uring_enter system call times = %d\n", systemTimes);
  return 0;
}
