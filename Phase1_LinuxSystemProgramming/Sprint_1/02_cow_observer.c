/* See COW in action */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

#define DATA_SIZE (4 * 1024 * 1024)  /* 4 MB of data */

int main(void)
{
    /* Allocate 4 MB — this allocates 1024 pages (4 KB each) */
    char *data = malloc(DATA_SIZE);
    if (!data) { perror("malloc"); return 1; }

    /* Touch every page so it's committed to physical RAM */
    memset(data, 'A', DATA_SIZE);

    /* Report memory before fork */
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "RssAnon:", 8) == 0)
            printf("  %s", line);
    }

    pid_t pid = fork();

    if (pid == 0) {
        /* CHILD: Check RSS — it should be the SAME as parent
         * (pages are shared via COW, no new physical memory allocated) */
        printf("=== CHILD AFTER FORK (before write) ===\n");
        f = fopen("/proc/self/status", "r");
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "RssAnon:", 8) == 0)
                printf("  %s", line);
        }
        fclose(f);

        /* NOW write to all pages — triggers COW on every single page */
        printf("[CHILD] Writing to all 4 MB...\n");
        memset(data, 'B', DATA_SIZE);

        /* Check RSS again — should now be DOUBLED */
        printf("=== CHILD AFTER WRITE (COW triggered) ===\n");
        f = fopen("/proc/self/status", "r");
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "RssAnon:", 8) == 0)
                printf("  %s", line);
        }
        fclose(f);

        _exit(0);
    }

    waitpid(pid, NULL, 0); //block the parent process to allow the child process to run.
    fclose(f);
    free(data);
    return 0;
}