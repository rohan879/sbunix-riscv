#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *curarg;
static int argidx;
static int argcnt;
static char **args;

static const char *nextarg(void) {
    if (argidx < argcnt)
        return args[argidx++];
    return "";
}

/* Process backslash escapes in the format string */
static void put_escaped(const char **pp) {
    const char *p = *pp;
    p++; /* skip backslash */
    switch (*p) {
    case 'n':  putchar('\n'); break;
    case 't':  putchar('\t'); break;
    case '\\': putchar('\\'); break;
    case 'a':  putchar('\a'); break;
    case 'b':  putchar('\b'); break;
    case 'f':  putchar('\f'); break;
    case 'r':  putchar('\r'); break;
    case 'v':  putchar('\v'); break;
    case '0': {
        /* Octal: \0NNN */
        int val = 0;
        p++;
        for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++, p++)
            val = val * 8 + (*p - '0');
        putchar(val);
        p--; /* will be incremented by caller */
        break;
    }
    default:
        putchar('\\');
        putchar(*p);
        break;
    }
    *pp = p;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        dprintf(2, "usage: printf FORMAT [ARGS...]\n");
        return 1;
    }

    const char *fmt = argv[1];
    args = argv + 2;
    argcnt = argc - 2;
    argidx = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            put_escaped(&p);
            continue;
        }

        if (*p == '%') {
            p++;
            if (*p == '\0') break;
            switch (*p) {
            case 's':
                curarg = nextarg();
                printf("%s", curarg);
                break;
            case 'd': {
                curarg = nextarg();
                long val = atol(curarg);
                printf("%ld", val);
                break;
            }
            case 'x': {
                curarg = nextarg();
                unsigned long val = strtoul(curarg, NULL, 0);
                printf("%lx", val);
                break;
            }
            case 'o': {
                curarg = nextarg();
                unsigned long val = strtoul(curarg, NULL, 0);
                printf("%lo", val);
                break;
            }
            case 'c': {
                curarg = nextarg();
                if (*curarg)
                    putchar(*curarg);
                break;
            }
            case '%':
                putchar('%');
                break;
            default:
                putchar('%');
                putchar(*p);
                break;
            }
            continue;
        }

        putchar(*p);
    }

    return 0;
}
