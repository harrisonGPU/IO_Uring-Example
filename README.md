# IO_Uring-Example

This repository contains a C program demonstrating the usage of Linux's `io_uring` interface for asynchronous I/O operations, designed specifically for Ubuntu 22.04.

## Prerequisites

Ensure your system is running Ubuntu 22.04. No additional libraries or packages are needed beyond what's provided by a standard C development environment.

## Building the Code

To build the program, follow these steps using `CMake`:

1. **Clone the Repository**

   Clone this repository to your local machine using the following command:

   ```bash
   git clone https://github.com/TSWorld1314/IO_Uring-Example.git
   cd IO_Uring-Example

   ```

2. **Create a Build Directory**

   Create a separate build directory for CMake:

   ```bash
   mkdir build
   cd build
   ```

3. **Configure the Project**

   Configure the project using CMake. Specify a debug build to include debug symbols:

   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Debug
   ```

4. **Compile the Project**

   Compile the project using the generated Makefile:

   ```bash
   cmake --build .
   ```

5. **Run the Program**

   After building the project, you can run the program directly:

   ```bash
   ./my_cat ../Data/*
   ```
   