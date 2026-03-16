#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static char addr_radix = 'o';  /* o, d, x, n */
static char out_type = 'o';    /* o, x, d, c */

static void print_addr(unsigned long off) {
    if (addr_radix == 'n') return;
    switch (addr_radix) {
    case 'o': printf("%07lo", off); break;
    case 'd': printf("%07ld", off); break;
    case 'x': printf("%07lx", off); break;
    }
}

static void print_byte(unsigned char b) {
    switch (out_type) {
    case 'o': printf(" %03o", b); break;
    case 'x': printf(" %02x", b); break;
    case 'd': printf(" %3d", b); break;
    case 'c':
        switch (b) {
        case '\0': printf("  \\0"); break;
        case '\n': printf("  \\n"); break;
        case '\t': printf("  \\t"); break;
        case '\r': printf("  \\r"); break;
        case '\a': printf("  \\a"); break;
        case '\b': printf("  \\b"); break;
        case '\f': printf("  \\f"); break;
        case '\v': printf("  \\v"); break;
        case '\\': printf("  \\\\"); break;
        default:
            if (b >= 0x20 && b <= 0x7e)
                printf("   %c", b);
            else
                printf(" %03o", b);
            break;
        }
        break;
    }
}

static void dump_fd(int fd, unsigned long *offset) {
    unsigned char buf[16];
    ssize_t n;

    while ((n = read(fd, buf, 16)) > 0) {
        print_addr(*offset);
        for (int i = 0; i < n; i++)
            print_byte(buf[i]);
        printf("\n");
        *offset += n;
    }
}

int main(int argc, char *argv[]) {
    int i = 1;

    /* Parse options */
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            addr_radix = argv[i + 1][0];
            i += 2;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            out_type = argv[i + 1][0];
            i += 2;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            dprintf(2, "od: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    unsigned long offset = 0;

    if (i >= argc) {
        /* Read from stdin */
        dump_fd(STDIN_FILENO, &offset);
    } else {
        for (; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                dprintf(2, "od: cannot open '%s'\n", argv[i]);
                return 1;
            }
            dump_fd(fd, &offset);
            close(fd);
        }
    }

    /* Print final offset */
    print_addr(offset);
    printf("\n");

    return 0;
}
