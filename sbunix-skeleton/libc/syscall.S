.global write
write:
  li a7, 1 # SYS_write
  ecall
  ret

.global read
read:
  li a7, 3 # SYS_read
  ecall
  ret

.global _exit
_exit:
  li a7, 2 # SYS_exit
  ecall
  # exit should not return
  1: j 1b
