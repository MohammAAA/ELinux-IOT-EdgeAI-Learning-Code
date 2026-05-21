/*
 * proc_info.c — Demonstrate process lifecycle and /proc filesystem
 *
 * Learning objectives:
 *   1. Read and parse /proc/self/status
 *   2. Fork a child process
 *   3. Observe PID/PPID relationship
 *   4. Use waitpid() to reap the child
 *   5. Show that child has a different PID but inherits the parent's PPID pattern
 *
 * Compile: gcc -Wall -Wextra -o 01_proc_info proc_info.c
 * Run:     ./proc_info
 */

#define _POSIX_C_SOURCE 200809L   /* For getopt, strdup in POSIX */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ============================================================
 * Read /proc/<pid>/status and print selected fields
 * ============================================================ */
void print_proc_info(const char *label)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/status");

    FILE *f = fopen(path, "r");
    if (!f) {
        perror("fopen /proc/self/status");
        return;
    }

    char line[256];
    printf("\n=== %s ===\n", label);
    printf("  Reading: %s\n", path);

    while (fgets(line, sizeof(line), f)) {
        /* Print the most useful fields for process inspection */
        if (strncmp(line, "Name:",    5) == 0 ||
            strncmp(line, "Pid:",     4) == 0 ||
            strncmp(line, "PPid:",    5) == 0 ||
            strncmp(line, "State:",   6) == 0 ||
            strncmp(line, "Threads:", 8) == 0 ||
            strncmp(line, "Uid:",     4) == 0 ||
            strncmp(line, "VmRSS:",   6) == 0) {
            printf("  %s", line);
        }
    }
    fclose(f);
}

/* ============================================================
 * Main: demonstrate fork + /proc observation
 * ============================================================ */
int main(void)
{
    /* ---- Step 1: Print parent's info BEFORE fork ---- */
    print_proc_info("PARENT (before fork)");
    printf("  Parent PID = %d\n", getpid());

    /* ---- Step 2: Fork ---- */
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* ===== CHILD PROCESS ===== */
        print_proc_info("CHILD (after fork)");

        /* Prove we're in a different process:
         * getpid() returns child's PID,
         * getppid() returns parent's PID
         */
        printf("  Child:  PID = %d, PPID = %d\n",
               getpid(), getppid());
        printf("  Child: sleeping for 2 seconds...\n");
        sleep(2);
        printf("  Child: exiting with code 42\n");
        _exit(42);   /* IMPORTANT: _exit(), not exit() */
    }

    /* ===== PARENT PROCESS ===== */
    printf("\n[PARENT] Fork successful. Child PID = %d\n", pid);
    sleep(10);
    // printf("[PARENT] Waiting for child to finish...\n");

    // int status;
    // pid_t waited = waitpid(pid, &status, 0);

    // if (waited == -1) {
    //     perror("waitpid");
    //     return 1;
    // }

    // /* ---- Step 3: Analyze child's exit status ---- */
    // if (WIFEXITED(status)) {
    //     printf("[PARENT] Child %d exited normally with status: %d\n",
    //            waited, WEXITSTATUS(status));
    // } else if (WIFSIGNALED(status)) {
    //     printf("[PARENT] Child %d was killed by signal: %d\n",
    //            waited, WTERMSIG(status));
    // }

    /* ---- Step 4: Print parent's info AFTER child exits ---- */
    print_proc_info("PARENT (after child exited)");
    printf("  Parent PID = %d (unchanged)\n", getpid());

    /* ---- Bonus: Verify child is gone via /proc ---- */
    char child_proc_path[64];
    snprintf(child_proc_path, sizeof(child_proc_path),
             "/proc/%d/status", pid);
    FILE *f = fopen(child_proc_path, "r");
    if (f == NULL) {
        printf("\n[VERIFIED] Child %d no longer exists in /proc "
               "(properly reaped)\n", pid);
    } else {
        printf("\n[WARNING] Child %d still exists in /proc! "
               "Zombie?\n", pid);
        fclose(f);
    }

    printf("\n=== DONE ===\n");
    return 0;
}