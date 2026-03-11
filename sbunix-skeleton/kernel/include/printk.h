#pragma once
void printk(const char *, ...);
void panic(const char *msg) __attribute__((noreturn));
int dmesg_read(char *buf, int bufsz, int offset);
int dmesg_total(void);
