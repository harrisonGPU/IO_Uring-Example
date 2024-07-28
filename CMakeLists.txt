cmake_minimum_required(VERSION 3.20)
project(my_io_project CXX)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Clang as the compiler
set(CMAKE_CXX_COMPILER "clang++")

# Add OpenMP and specific GPU architecture compile options
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp --offload-arch=sm_86")

# Create a library for device-specific functions
add_library(device STATIC device.cpp)

# Apply the -ffreestanding flag specifically to the device library
set_target_properties(device PROPERTIES COMPILE_FLAGS "-ffreestanding")

# Create another library for host-specific functions
add_library(host STATIC host.cpp)

# Add an executable
add_executable(my_cat main.cpp)

# Link libraries to the executable
target_link_libraries(my_cat device host)

