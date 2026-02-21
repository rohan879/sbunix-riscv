#include <stdio.h>

int main(int argc, char *argv[]) {
    puts("SBUnix Shell started.");
    puts("Type characters to echo them back. Type 'q' to exit.");

    while (1) {
        printf("$ "); // Note: printf might still be a stub, using puts for now
        // puts("$ "); 
        int c = getchar();
        if (c == 'q') {
            puts("\nExiting shell.");
            break;
        }
        if (c != -1) {
            // Echo character
            char buf[2];
            buf[0] = (char)c;
            buf[1] = '\0';
            puts(buf);
        }
    }
    return 0;
}