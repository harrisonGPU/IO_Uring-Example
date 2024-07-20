cmake_minimum_required(VERSION 3.20)
project(my_io_project CXX)  # Change the project language to C++

# Set the C++ standard
set(CMAKE_CXX_STANDARD 11)  # Updated to C++11 as mentioned in comments
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force the use of Clang as the compiler
set(CMAKE_CXX_COMPILER "clang++")

# Add OpenMP and specific GPU architecture compile options
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp --offload-arch=sm_86")

# Add static library
add_library(my_io STATIC my_io.cpp)  # Ensure your library uses the .cpp file

# Add executable
add_executable(my_cat main.cpp)  # Ensure your executable is set to use .cpp

# Link libraries
target_link_libraries(my_cat my_io)

# If use std::thread or other threading facilities
# set(THREADS_PREFER_PTHREAD_FLAG ON)
# find_package(Threads REQUIRED)
# target_link_libraries(my_cat Threads::Threads)
