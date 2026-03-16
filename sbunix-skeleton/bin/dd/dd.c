#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static unsigned long parse_num(const char *s) {
    char *end;
    unsigned long n = strtoul(s, &end, 0);
    /* Support common suffixes */
    if (*end == 'k' || *end == 'K') n *= 1024;
    else if (*end == 'm' || *end == 'M') n *= 1024 * 1024;
    return n;
}

int main(int argc, char *argv[]) {
    char *infile = NULL;
    char *outfile = NULL;
    unsigned long bs = 512;
    unsigned long count = 0;   /* 0 means copy all */
    unsigned long skip = 0;
    unsigned long seek_n = 0;
    int count_set = 0;

    /* Parse operands */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "if=", 3) == 0)
            infile = argv[i] + 3;
        else if (strncmp(argv[i], "of=", 3) == 0)
            outfile = argv[i] + 3;
        else if (strncmp(argv[i], "bs=", 3) == 0)
            bs = parse_num(argv[i] + 3);
        else if (strncmp(argv[i], "count=", 6) == 0) {
            count = parse_num(argv[i] + 6);
            count_set = 1;
        } else if (strncmp(argv[i], "skip=", 5) == 0)
            skip = parse_num(argv[i] + 5);
        else if (strncmp(argv[i], "seek=", 5) == 0)
            seek_n = parse_num(argv[i] + 5);
        else {
            dprintf(2, "dd: unknown operand '%s'\n", argv[i]);
            return 1;
        }
    }

    /* Open input */
    int ifd = STDIN_FILENO;
    if (infile) {
        ifd = open(infile, O_RDONLY);
        if (ifd < 0) {
            dprintf(2, "dd: cannot open '%s': %s\n", infile, strerror(errno));
            return 1;
        }
    }

    /* Open output */
    int ofd = STDOUT_FILENO;
    if (outfile) {
        ofd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ofd < 0) {
            dprintf(2, "dd: cannot open '%s': %s\n", outfile, strerror(errno));
            return 1;
        }
    }

    /* Skip input blocks */
    if (skip > 0)
        lseek(ifd, (off_t)(skip * bs), SEEK_SET);

    /* Seek output blocks */
    if (seek_n > 0)
        lseek(ofd, (off_t)(seek_n * bs), SEEK_SET);

    /* Allocate buffer */
    char *buf = malloc(bs);
    if (!buf) {
        dprintf(2, "dd: out of memory\n");
        return 1;
    }

    unsigned long full_in = 0, full_out = 0;
    unsigned long partial_in = 0, partial_out = 0;
    unsigned long blocks_done = 0;

    while (!count_set || blocks_done < count) {
        ssize_t n = read(ifd, buf, bs);
        if (n <= 0) break;

        if ((unsigned long)n == bs) full_in++;
        else partial_in++;

        ssize_t w = write(ofd, buf, n);
        if (w < 0) {
            dprintf(2, "dd: write error: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
        if ((unsigned long)w == bs) full_out++;
        else partial_out++;

        blocks_done++;
    }

    free(buf);
    if (infile) close(ifd);
    if (outfile) close(ofd);

    dprintf(2, "%lu+%lu records in\n", full_in, partial_in);
    dprintf(2, "%lu+%lu records out\n", full_out, partial_out);

    return 0;
}
