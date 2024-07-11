#!/bin/bash

# src
SOURCE_FILE="/root/Projects/io_uring/IO_Uring-Example/Data/large.txt"

# dest
TARGET_DIR="/root/Projects/io_uring/IO_Uring-Example/Data/Large"

mkdir -p "$TARGET_DIR"

# copy
for i in {1..100}; do
    cp "$SOURCE_FILE" "$TARGET_DIR/$i.txt"
    echo "File $i.txt copied"
done

echo "All files have been copied successfully."
