#!/bin/bash

# 设置源文件路径
SOURCE_FILE="/root/Projects/io_uring/IO_Uring-Example/Data/Large-Data1.txt"

# 设置目标目录路径
TARGET_DIR="/root/Projects/io_uring/IO_Uring-Example/Data/Large"

# 检查目标目录是否存在，如果不存在则创建
mkdir -p "$TARGET_DIR"

# 开始复制文件
for i in {1..100}; do
    cp "$SOURCE_FILE" "$TARGET_DIR/$i.txt"
    echo "File $i.txt copied"
done

echo "All files have been copied successfully."
