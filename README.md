# Unix File System

This project implements a simple file system (using inodes) on top of a virtual disk.

The virtual disk has 8,192 blocks and is a single file stored on the "real" file system provided by the Linux OS.

Each block holds 4KB.

## Layout of blocks

First block: super block

Second block: directory table

Third block: inode bitmap

Fourth block: data bitmap

Fifth block: inode table

Remaining: data blocks

## Configuration

Max file size supported: 20MB

## Test files

1. test_make_fs
2. test_mount_umount
3. test_fs_create
4. test_listfiles
5. test_fs_write
6. test_getfilesize
7. test_fs_read
8. test_open_close
9. test_fs_delete
10. test_truncate
