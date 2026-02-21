#include <tar.h>
#include <printk.h>

// --- Static implementations of standard library functions ---

static int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

// --- End of standard library implementations ---

// Converts an octal string to a long integer.
long oct2bin(char *str, int size) {
    long n = 0;
    char *c = str;
    while (c < str + size) {
        if (*c == 0) break;
        if (*c == ' ') {
            c++;
            continue;
        }
        n = n * 8 + (*c - '0');
        c++;
    }
    return n;
}

// Retrieves the file size from a tar header.
size_t tar_get_file_size(struct posix_header *header) {
    return oct2bin(header->size, sizeof(header->size));
}

// Checks the checksum of a tar header.
int tar_check_checksum(struct posix_header *header) {
    unsigned int i;
    unsigned int sum = 0;
    unsigned int stored_chksum;

    char *ptr = (char *)header;
    for (i = 0; i < sizeof(struct posix_header); i++) {
        if (i >= __builtin_offsetof(struct posix_header, chksum) && i < __builtin_offsetof(struct posix_header, chksum) + sizeof(header->chksum)) {
            sum += ' '; // Treat chksum field as spaces (ASCII 0x20)
        } else {
            sum += (unsigned char)ptr[i];
        }
    }

    stored_chksum = oct2bin(header->chksum, sizeof(header->chksum));

    return (sum == stored_chksum);
}

// Iterate through the tar archive and list its contents.
void tar_ls(char *tar_start, char *tar_end) {
    char *current_ptr = tar_start;
    struct posix_header *header;
    size_t file_size;

    printk("Listing embedded tarfs contents:\n");

    while (current_ptr < tar_end) {
        header = (struct posix_header *)current_ptr;

        if (header->name[0] == '\0') {
            printk("End of tar archive.\n");
            break; // End of archive
        }

        if ((strncmp(header->magic, "ustar", 5) != 0) || !tar_check_checksum(header)) {
            current_ptr += 512;
            continue;
        }

        printk("  %s (size: %ld)\n", header->name, (long)tar_get_file_size(header));

        file_size = tar_get_file_size(header);
        current_ptr += sizeof(struct posix_header);
        current_ptr += ((file_size + 511) / 512) * 512;
    }
}

// Find a file in the tar archive and return a pointer to its header.
struct posix_header *tar_find_file(char *tar_start, char *tar_end, const char *filename) {
    char *current_ptr = tar_start;
    struct posix_header *header;
    size_t file_size;

    while (current_ptr < tar_end) {
        header = (struct posix_header *)current_ptr;

        if (header->name[0] == '\0') {
            break; // End of archive
        }

        if ((strncmp(header->magic, "ustar", 5) != 0) || !tar_check_checksum(header)) {
            current_ptr += 512;
            continue;
        }
        
        // Compare filename, note: tar filenames are null-terminated within 100 chars
        if (strncmp(header->name, filename, 100) == 0) {
            return header;
        }

        file_size = tar_get_file_size(header);
        current_ptr += sizeof(struct posix_header);
        current_ptr += ((file_size + 511) / 512) * 512;
    }

    return NULL; // File not found
}
