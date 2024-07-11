cmake_minimum_required(VERSION 3.10)
project(my_io_project C)

set(CMAKE_C_STANDARD 99)

add_library(my_io STATIC my_io.c)

add_executable(my_cat main.c)

target_link_libraries(my_cat my_io)
