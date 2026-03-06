#include <stdio.h>
#include <unistd.h>

static unsigned int simple_atou(const char *s) {
    unsigned int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        write(2, "Usage: sleep SECONDS\n", 21);
        return 1;
    }
    unsigned int secs = simple_atou(argv[1]);
    sleep(secs);
    return 0;
}
