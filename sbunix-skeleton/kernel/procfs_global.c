#include <procfs_internal.h>
#include <kalloc.h>
#include <printk.h>
#include <string.h>
#include <trap_internal.h>
#include <vfs.h>
#include <virtio_blk.h>

static int append_char(char *buf, int bufsz, int pos, char c) {
  if (pos < bufsz)
    buf[pos++] = c;
  return pos;
}

static int append_str(char *buf, int bufsz, int pos, const char *s) {
  int len = strlen(s);
  if (pos >= bufsz)
    return pos;
  if (pos + len > bufsz)
    len = bufsz - pos;
  if (len > 0) {
    memcpy(buf + pos, s, len);
    pos += len;
  }
  return pos;
}

static int append_int(char *buf, int bufsz, int pos, long v) {
  char tmp[24];
  int ti = 0;
  if (v < 0) {
    pos = append_char(buf, bufsz, pos, '-');
    v = -v;
  }
  if (v == 0) {
    tmp[ti++] = '0';
  } else {
    while (v > 0 && ti < (int)sizeof(tmp)) {
      tmp[ti++] = (char)('0' + (v % 10));
      v /= 10;
    }
  }
  while (ti > 0)
    pos = append_char(buf, bufsz, pos, tmp[--ti]);
  return pos;
}

int procfs_global_is_dynamic(enum procfs_type type) {
  return type == PROCFS_KMSG || type == PROCFS_PARTITIONS ||
         type == PROCFS_STAT_GLOBAL || type == PROCFS_INTERRUPTS ||
         type == PROCFS_SLABINFO || type == PROCFS_BUDDYINFO;
}

static int render_filesystems(char *buf, int bufsz) {
  return append_str(buf, bufsz, 0,
                    "nodev\tdevfs\n"
                    "nodev\tproc\n"
                    "nodev\tramfs\n"
                    "nodev\tsysfs\n"
                    "tarfs\n"
                    "ext2\n");
}

static int render_devices(char *buf, int bufsz) {
  return append_str(buf, bufsz, 0,
                    "Character devices:\n"
                    "  1 mem\n"
                    "  4 tty\n"
                    "  5 console\n"
                    " 10 misc\n"
                    "\n"
                    "Block devices:\n"
                    "254 virtblk\n");
}

static int render_partitions(char *buf, int bufsz) {
  int pos = 0;
  int count = virtio_blk_count();

  pos = append_str(buf, bufsz, pos,
                   "major minor  #blocks  name\n\n");
  for (int i = 0; i < count; i++) {
    uint64_t sectors = virtio_blk_capacity_on_dev(i);
    pos = append_str(buf, bufsz, pos, " 254 ");
    pos = append_int(buf, bufsz, pos, i);
    pos = append_char(buf, bufsz, pos, ' ');
    pos = append_int(buf, bufsz, pos, (long)(sectors / 2));
    pos = append_str(buf, bufsz, pos, " vda");
    if (i > 0)
      pos = append_char(buf, bufsz, pos, (char)('0' + i));
    pos = append_char(buf, bufsz, pos, '\n');
  }
  return pos;
}

static int render_stat(char *buf, int bufsz) {
  int running = 0;
  int blocked = 0;
  int total = 0;
  int pos = 0;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    total++;
    if (p->state == RUNNABLE || p->state == RUNNING)
      running++;
    if (p->state == SLEEPING)
      blocked++;
  }

  pos = append_str(buf, bufsz, pos, "cpu  ");
  pos = append_int(buf, bufsz, pos, (long)ticks);
  pos = append_str(buf, bufsz, pos, " 0 0 ");
  pos = append_int(buf, bufsz, pos, (long)ticks);
  pos = append_str(buf, bufsz, pos, " 0 0 0 0 0 0\nintr ");
  pos = append_int(buf, bufsz, pos,
                   (long)(procfs_diag_timer_interrupts +
                          procfs_diag_external_interrupts));
  pos = append_str(buf, bufsz, pos, " 0 0\nctxt 0\nbtime 0\nprocesses ");
  pos = append_int(buf, bufsz, pos, nextpid > 1 ? nextpid - 1 : 0);
  pos = append_str(buf, bufsz, pos, "\nprocs_running ");
  pos = append_int(buf, bufsz, pos, running);
  pos = append_str(buf, bufsz, pos, "\nprocs_blocked ");
  pos = append_int(buf, bufsz, pos, blocked);
  pos = append_str(buf, bufsz, pos, "\nsoftirq 0\n");
  (void)total;
  return pos;
}

static int render_slabinfo(char *buf, int bufsz) {
  int pos = append_str(buf, bufsz, 0,
    "slabinfo - version: 2.1\n"
    "# name <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>\n");
  int sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
  for (int i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
    pos = append_str(buf, bufsz, pos, "kmalloc-");
    pos = append_int(buf, bufsz, pos, sizes[i]);
    pos = append_str(buf, bufsz, pos, " 0 0 ");
    pos = append_int(buf, bufsz, pos, sizes[i]);
    pos = append_str(buf, bufsz, pos, " 0 1 : tunables 0 0 0 : slabdata 0 0 0\n");
  }
  return pos;
}

static int render_buddyinfo(char *buf, int bufsz) {
  int pos = 0;
  pos = append_str(buf, bufsz, pos, "Node 0, zone   Normal ");
  pos = append_int(buf, bufsz, pos, get_free_page_count());
  for (int i = 0; i < 10; i++)
    pos = append_str(buf, bufsz, pos, " 0");
  pos = append_char(buf, bufsz, pos, '\n');
  return pos;
}

int procfs_render_global(enum procfs_type type, char *buf, int bufsz) {
  int pos = 0;

  switch (type) {
  case PROCFS_KMSG:
    return dmesg_read(buf, bufsz, 0);
  case PROCFS_FILESYSTEMS:
    return render_filesystems(buf, bufsz);
  case PROCFS_CMDLINE_GLOBAL:
    return append_str(buf, bufsz, 0, "root=tarfs console=ttyS0\n");
  case PROCFS_DEVICES:
    return render_devices(buf, bufsz);
  case PROCFS_PARTITIONS:
    return render_partitions(buf, bufsz);
  case PROCFS_CONSOLES:
    return append_str(buf, bufsz, 0, "ttyS0                -W- (EC p  )    4:64\n");
  case PROCFS_TTY_DRIVERS:
    return append_str(buf, bufsz, 0,
                      "/dev/tty             /dev/tty        5 0 system:/dev/tty\n"
                      "serial               /dev/ttyS       4 64-64 serial\n"
                      "pty_slave            /dev/pts      136 0-0 pty:slave\n"
                      "pty_master           /dev/ptmx       5 2 system\n");
  case PROCFS_TTY_LDISCS:
    return append_str(buf, bufsz, 0, "n_tty       0\n");
  case PROCFS_STAT_GLOBAL:
    return render_stat(buf, bufsz);
  case PROCFS_INTERRUPTS:
    pos = append_str(buf, bufsz, pos, "           CPU0\n");
    pos = append_str(buf, bufsz, pos, "  5: ");
    pos = append_int(buf, bufsz, pos, (long)procfs_diag_timer_interrupts);
    pos = append_str(buf, bufsz, pos, "  RISC-V timer\n 10: ");
    pos = append_int(buf, bufsz, pos, (long)procfs_diag_uart_interrupts);
    pos = append_str(buf, bufsz, pos, "  PLIC uart\n");
    return pos;
  case PROCFS_SLABINFO:
    return render_slabinfo(buf, bufsz);
  case PROCFS_BUDDYINFO:
    return render_buddyinfo(buf, bufsz);
  default:
    return 0;
  }
}
