#ifndef _PROC_H
#define _PROC_H

#include <riscv.h>

// Saved registers for kernel context switches.
struct context {
  uint64_t ra;
  uint64_t sp;

  // Callee-saved
  uint64_t s0;
  uint64_t s1;
  uint64_t s2;
  uint64_t s3;
  uint64_t s4;
  uint64_t s5;
  uint64_t s6;
  uint64_t s7;
  uint64_t s8;
  uint64_t s9;
  uint64_t s10;
  uint64_t s11;
};

// Trapframe matches the layout of registers saved on stack in trap.S
struct trapframe {
  /*   0 */ uint64_t ra;
  /*   8 */ uint64_t t0;
  /*  16 */ uint64_t t1;
  /*  24 */ uint64_t t2;
  /*  32 */ uint64_t t3;
  /*  40 */ uint64_t t4;
  /*  48 */ uint64_t t5;
  /*  56 */ uint64_t t6;
  /*  64 */ uint64_t a0;
  /*  72 */ uint64_t a1;
  /*  80 */ uint64_t a2;
  /*  88 */ uint64_t a3;
  /*  96 */ uint64_t a4;
  /* 104 */ uint64_t a5;
  /* 112 */ uint64_t a6;
  /* 120 */ uint64_t a7;
  /* 128 */ uint64_t s0;
  /* 136 */ uint64_t s1;
  /* 144 */ uint64_t s2;
  /* 152 */ uint64_t s3;
  /* 160 */ uint64_t s4;
  /* 168 */ uint64_t s5;
  /* 176 */ uint64_t s6;
  /* 184 */ uint64_t s7;
  /* 192 */ uint64_t s8;
  /* 200 */ uint64_t s9;
  /* 208 */ uint64_t s10;
  /* 216 */ uint64_t s11;
  /* 224 */ uint64_t gp;
  /* 232 */ uint64_t tp;
  /* 240 */ uint64_t sp;
  /* 248 */ uint64_t epc; // We can use the extra space for SEPC if we save it
};

#endif
