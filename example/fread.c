#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef struct {
    int fd;             // 文件描述符
    size_t pos;         // 当前读取位置，用于模拟 ftell
} MY_FILE;

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

// 模拟 fread
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

// 模拟 fclose
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

    while ((bytes_read = my_fread(buffer, 1, sizeof(buffer), file)) > 0) {
        write(STDOUT_FILENO, buffer, bytes_read);
    }

    my_fclose(file);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        my_cat(argv[i]);
    }

    return 0;
}
