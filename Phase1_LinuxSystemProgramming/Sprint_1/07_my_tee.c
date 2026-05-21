/*
 * 07_my_tee.c
 * Read from stdin, write to stdout AND a file
 * Like the Unix 'tee' command, using only syscalls
 * This demonstrates working with multiple FDs simultaneously:
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#define BUF_SIZE 4096

static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t total = 0;
    while (total < count) {
        ssize_t n = write(fd, (const char *)buf + total,
                          count - total);
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return (ssize_t)total;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <output_file>\n", argv[0]);
        return 1;
    }

    /* Open output file */
    int out_fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd == -1) {
        perror(argv[1]);
        return 1;
    }

    /* Read from stdin (FD 0), write to stdout (FD 1) and file */
    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        if (write_all(STDOUT_FILENO, buf, (size_t)n) == -1) {
            perror("write stdout");
            close(out_fd);
            return 1;
        }
        if (write_all(out_fd, buf, (size_t)n) == -1) {
            perror("write file");
            close(out_fd);
            return 1;
        }
    }

    if (n == -1) {
        perror("read stdin");
        close(out_fd);
        return 1;
    }

    close(out_fd);
    return 0;
}

/* How to test:

# Pipe data through it
echo "Hello World" | ./my_tee /tmp/tee_output
cat /tmp/tee_output

# Chain it with other commands
ls -la | ./my_tee /tmp/dir_listing.txt | wc -l
cat /tmp/dir_listing.txt
*/