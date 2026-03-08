#pragma once
void printk(const char *, ...);
int dmesg_read(char *buf, int bufsz, int offset);
int dmesg_total(void);
