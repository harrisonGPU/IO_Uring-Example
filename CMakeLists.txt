cmake_minimum_required(VERSION 3.10)
project(my_io_project C)

# Set the C standard
set(CMAKE_C_STANDARD 99)

# Force the use of the Clang compiler
set(CMAKE_C_COMPILER "clang")

# Add OpenMP and specific GPU architecture compile options
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fopenmp --offload-arch=sm_86")

# Add static library
add_library(my_io STATIC my_io.c)

# Add executable
add_executable(my_cat main.c)

# Link libraries
target_link_libraries(my_cat my_io)
