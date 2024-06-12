cmake_minimum_required(VERSION 3.10)
project(io_uring_example)

# Set C standard to C11
set(CMAKE_C_STANDARD 11)

# Find liburing package using pkg-config
find_package(PkgConfig)
pkg_check_modules(LIBURING QUIET liburing)

# Check if liburing was found
if(NOT LIBURING_FOUND)
    message(FATAL_ERROR "liburing not found. Please install liburing.")
endif()

# Create an executable from main.c
add_executable(io_uring_example main.c)

# Include directories for liburing
target_include_directories(io_uring_example PRIVATE ${LIBURING_INCLUDE_DIRS})

# Link liburing to the executable
target_link_libraries(io_uring_example ${LIBURING_LIBRARIES})

# Set the linker flags from pkg-config
target_link_options(io_uring_example PRIVATE ${LIBURING_LDFLAGS})
