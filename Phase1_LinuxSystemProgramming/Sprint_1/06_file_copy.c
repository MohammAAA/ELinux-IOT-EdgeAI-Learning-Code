/*
 * 06_file_copy.c — Copy a file using only POSIX syscalls
 *                (no fopen/fread/fwrite)
 *
 * Usage: ./06_file_copy <source> <destination>
 *
 * Learning objectives:
 *   1. Use open() with correct flags
 *   2. Handle partial reads with a loop
 *   3. Handle partial writes with a loop
 *   4. Handle EINTR from signals
 *   5. Report errors with perror()
 *   6. Preserve file permissions
 *   7. Use close() correctly
 *
 * Compile: gcc -Wall -Wextra -o file_copy 06_file_copy.c
 */

#define _POSIX_C_SOURCE 200809L
/*
The _POSIX_C_SOURCE is a feature test macro.
It tells the compiler to make symbols, functions, and types required by the POSIX.1-2008 standard visible in the header files.
Why is it needed?
    Standard C headers (like stdio.h or stdlib.h) only define functions that are part of the ISO C standard (e.g., printf).
    If we want to use Unix-specific functions that aren't in the standard C library—like getline(), strndup(), or the signalfd and timerfd, we must define this macro before including any headers.
Key Details:
    Version Meaning: The value 200809L refers to the year (2008) and month (09) the standard was finalized.
    Placement: It must be defined at the very top of the source file, before any #include statements.
    Strict Mode: When we compile with strict standards (like gcc -std=c11), most POSIX functions are hidden unless we explicitly enable them with this macro.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#define BUF_SIZE 65536  /* 64 KB — good balance for disk I/O */

/* ============================================================
 * Write all bytes, handling short writes and EINTR
 * ============================================================ */
static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t total = 0;
    while (total < count) {
        ssize_t n = write(fd, (const char *)buf + total,
                          count - total);
        if (n == -1) {
            if (errno == EINTR)
                continue;       /* Signal — retry */
            return -1;          /* Real error */
        }
        total += n;
    }
    return (ssize_t)total;
}

/* ============================================================
 * Copy file: src_fd → dst_fd
 * Returns: bytes copied on success, -1 on error
 * ============================================================ */
static off_t copy_data(int src_fd, int dst_fd)
{
    char buf[BUF_SIZE];
    off_t total_copied = 0;

    while (1) {
        ssize_t n_read = read(src_fd, buf, sizeof(buf));

        if (n_read == -1) {
            if (errno == EINTR)
                continue;       /* Signal — retry */
            perror("read");
            return -1;
        }

        if (n_read == 0)
            break;              /* EOF — done */

        /* Write exactly what we read immediately */
        ssize_t n_written = write_all(dst_fd, buf, (size_t)n_read);
        if (n_written == -1) {
            perror("write");
            return -1;
        }

        total_copied += n_written;
    }

    return total_copied;
}

/* ============================================================
 * Main
 * ============================================================ */
int main(int argc, char *argv[])
{
    /* ---- Argument check ---- */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source> <destination>\n", argv[0]);
        return 1;
    }

    const char *src_path  = argv[1];
    const char *dst_path  = argv[2];

    /* ---- Open source file (read-only) ---- */
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd == -1) {
        perror(src_path);
        return 1;
    }

    /* ---- Get source file permissions (to preserve on destination) ---- */
    struct stat st;
    if (fstat(src_fd, &st) == -1) {
        perror("fstat");
        close(src_fd);
        return 1;
    }

    /* ---- Open destination file (create, write-only, truncate) ---- */
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst_fd == -1) {
        perror(dst_path);
        close(src_fd);
        return 1;
    }

    /* ---- Copy data ---- */
    printf("Copying: %s -> %s\n", src_path, dst_path);
    off_t copied = copy_data(src_fd, dst_fd);

    if (copied == -1) {
        fprintf(stderr, "Copy failed ... try again \n");
        close(src_fd);
        close(dst_fd);
        return 1;
    }

    /* ---- Flush and close destination first (writes may be buffered) ---- */
    if (close(dst_fd) == -1) {
        perror("close destination");
        close(src_fd);
        return 1;
    }

    /* ---- Close source ---- */
    if (close(src_fd) == -1) {
        perror("close source");
        return 1;
    }

    /* ---- Report ---- */
    printf("Copied %ld bytes (%.2f KB)\n",
           (long)copied, (double)copied / 1024.0);

    return 0;
}

/* How to test:
# Compile
gcc -Wall -Wextra -o file_copy 06_file_copy.c

# Test 1: Copy a regular file
./file_copy /etc/passwd /tmp/passwd_copy
diff /etc/passwd /tmp/passwd_copy
echo "Exit code: $?"
# Expected: no diff output, exit code 0

# Test 2: Copy a binary file
./file_copy /usr/bin/ls /tmp/ls_copy
cmp /usr/bin/ls /tmp/ls_copy
echo "Exit code: $?"

# Test 3: Copy a large file (test buffer handling)
dd if=/dev/urandom of=/tmp/large_file bs=1M count=10
./file_copy /tmp/large_file /tmp/large_file_copy
md5sum /tmp/large_file /tmp/large_file_copy
# Both MD5 sums should match

# Test 4: Error cases
./file_copy /nonexistent /tmp/out          # Should print error
./file_copy /etc/passwd /proc/nonexistent  # Should print error

# Test 5: Verify permissions are preserved
ls -la /etc/passwd /tmp/passwd_copy
# Should have the same permissions

# Test 6: Empty file
touch /tmp/empty_file
./file_copy /tmp/empty_file /tmp/empty_copy
ls -la /tmp/empty_copy   # Should be 0 bytes
*/