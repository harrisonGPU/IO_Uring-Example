#include "my_io.h"
#include <iostream>
#include <ctime>
#include <memory>
#include <unistd.h>
#include <omp.h>

double perFileTime;

void cat(const char *filename) {
    my_file *mf = my_fopen(filename, "r");
    if (!mf) {
        perror("Failed to open file.");
        return;
    }

    size_t bytesRead;
    std::unique_ptr<char[]> buffer(new char[4096]);
    auto start = std::clock();

    while ((bytesRead = my_fread(buffer.get(), sizeof(char), sizeof(buffer), mf)) > 0) {
        write(STDOUT_FILENO, buffer.get(), bytesRead);
    }

    //std::cout << std::endl;
    auto end = std::clock();
    double cpu_time_used = double(end - start) / CLOCKS_PER_SEC;
    perFileTime += cpu_time_used;

    std::cout << "\nCompleted reading '" << filename << "': Duration = " << cpu_time_used << " seconds, File Size = " << mf->fi->file_sz << " bytes.\n";
    my_fclose(mf);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <filename>\n";
        return 1;
    }

    perFileTime = 0.0;
    auto start = std::clock();

    for (int i = 1; i < argc; i++) {
        cat(argv[i]);
    }

    auto end = std::clock();
    double total_time = double(end - start) / CLOCKS_PER_SEC;

    //std::cout << "Total time to read " << (argc - 1) << " files: " << total_time << " seconds\n";
    //std::cout << "Average time per read file: " << perFileTime / (argc - 1) << " seconds\n";
    //std::cout << "Use io_uring_enter system call times = " << systemTimes << std::endl;

    return 0;
}
