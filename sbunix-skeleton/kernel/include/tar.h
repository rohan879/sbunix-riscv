#ifndef _TAR_H
#define _TAR_H

#include <riscv.h>
#define NULL ((void *)0)

struct posix_header {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char chksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char padding[12];
};

long oct2bin(char *str, int size);
size_t tar_get_file_size(struct posix_header *header);
int tar_check_checksum(struct posix_header *header);

void tar_ls(char *tar_start, char *tar_end);
struct posix_header *tar_find_file(char *tar_start, char *tar_end, const char *filename);

#endif