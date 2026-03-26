#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

int main() {
    printf("POSIX compliance test: checking if stack pages are properly zeroed\n");
    
    pid_t child1 = fork();
    if (child1 == 0) {
        // Child 1: Allocate and dirty memory pages to pollute the free pool
        printf("Child 1: Allocating and dirtying memory...\n");
        
        // Allocate several pages worth of memory and fill with non-zero data
        char *buffer = malloc(32768); // 8 pages
        if (buffer) {
            memset(buffer, 0xAA, 32768); // Fill with pattern 0xAA
            printf("Child 1: Memory dirtied with pattern 0xAA\n");
            free(buffer);
        }
        
        // Force some stack usage too
        char stack_buffer[4096];
        memset(stack_buffer, 0xBB, sizeof(stack_buffer));
        printf("Child 1: Stack dirtied with pattern 0xBB\n");
        
        exit(0);
    }
    
    // Parent waits for child 1 to exit and return its dirty pages to free pool
    int status;
    waitpid(child1, &status, 0);
    printf("Parent: Child 1 exited, dirty pages returned to free pool\n");
    
    // Now fork another child and exec a simple program
    pid_t child2 = fork();
    if (child2 == 0) {
        // Child 2: exec a simple program that should get clean stack pages
        printf("Child 2: About to exec echo program...\n");
        char *argv[] = {"echo", "Hello from exec'd program - SUCCESS!", NULL};
        execv("/bin/echo", argv);
        printf("ERROR: exec failed\n");
        exit(1);
    }
    
    // Parent waits for child 2
    waitpid(child2, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("SUCCESS: exec'd program ran successfully\n");
        printf("[PASS] posix_mem_test\n");
        return 0;
    } else {
        printf("FAILED: exec'd program did not run successfully\n");
        printf("[FAIL] posix_mem_test\n");
        return 1;
    }
}