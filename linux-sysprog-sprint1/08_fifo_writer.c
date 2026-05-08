/*
 * 08_fifo_writer.c — Write messages to a named pipe (FIFO)
 *
 * Usage: ./fifo_writer [message_count]
 *   Default: sends 10 messages
 *
 * This program opens the FIFO for writing (blocking — waits for a reader)
 * and sends numbered log messages.
 *
 * Compile: gcc -Wall -Wextra -o fifo_writer 08_fifo_writer.c
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#define FIFO_PATH "/tmp/logd.fifo"
#define MSG_SIZE  256

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
                continue;
            return -1;
        }
        total += n;
    }
    return (ssize_t)total;
}

/* ============================================================
 * Get current timestamp string (ISO 8601-like)
 * ============================================================ */
static void get_timestamp(char *buf, size_t bufsz)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, bufsz, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* ============================================================
 * Main
 * ============================================================ */
int main(int argc, char *argv[])
{
    int count = 10;
    if (argc > 1) {
        count = atoi(argv[1]);
        if (count <= 0) count = 10;
    }

    printf("[WRITER] Opening FIFO: %s (will block until reader connects)\n",
           FIFO_PATH);

    /* Create FIFO */       
    if (mkfifo(FIFO_PATH, 0666) == -1) {
        if (errno != EEXIST) { // Ignore error if it already exists
            perror("mkfifo");
            return 1;
        }
    }

    /* Open FIFO for writing — BLOCKS until a reader opens it too */
    int fd = open(FIFO_PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (fd == -1) {
        perror("open fifo");
        return 1;
    }

    printf("[WRITER] Connected! Sending %d messages...\n", count);

    char msg[MSG_SIZE];
    char timestamp[32];

    for (int i = 1; i <= count; i++) {
        get_timestamp(timestamp, sizeof(timestamp));

        /* Build a log message with timestamp and sequence number */
        int len = snprintf(msg, sizeof(msg),
            "[%s] [%05d] INFO Log message number %d from writer PID %d\n",
            timestamp, i, i, getpid());

        if (len >= (int)sizeof(msg)) {
            len = (int)sizeof(msg) - 1;
        }

        /* Write the complete message */
        ssize_t n = write_all(fd, msg, (size_t)len);
        if (n == -1) {
            if (errno == EPIPE) {
                fprintf(stderr, "[WRITER] Reader disconnected! "
                        "(Broken pipe)\n");
            } else {
                perror("write");
            }
            close(fd);
            return 1;
        }

        printf("[WRITER] Sent message %d/%d (%zd bytes)\n", i, count, n);

        /* Small delay between messages (optional, for demo purposes) */
        sleep(1);  /* 1 s */
    }

    printf("[WRITER] All %d messages sent. Closing FIFO.\n", count);
    close(fd);

    return 0;
}