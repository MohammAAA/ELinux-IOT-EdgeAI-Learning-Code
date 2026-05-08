/*
 * 09_fifo_reader.c — Read messages from a named pipe (FIFO) using poll()
 *
 * Usage: ./fifo_reader
 *
 * Opens the FIFO with O_NONBLOCK (so open() doesn't block forever
 * if no writer exists), then uses poll() to wait for data efficiently.
 *
 * Compile: gcc -Wall -Wextra -o fifo_reader 09_fifo_reader.c
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FIFO_PATH    "/tmp/logd.fifo"
#define READ_BUF_SIZE 4096
#define POLL_TIMEOUT  5000  /* 5 seconds — check for shutdown periodically */

/* ============================================================
 * Volatile flag for clean shutdown (set by signal handler)
 * We'll use this properly on Saturday — for now, Ctrl+C works.
 * ============================================================ */
static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
    /* write() is async-signal-safe, printf() is NOT */
    const char msg[] = "[READER] SIGINT received, shutting down...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void)
{
    /* ---- Set up SIGINT handler (Ctrl+C) ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* ---- Ensure the FIFO exists ---- */
    /* mkfifo returns EEXIST if it already exists — that's fine */
    if (mkfifo(FIFO_PATH, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        return 1;
    }

    /* ---- Open FIFO with O_NONBLOCK ----
     *
     * WHY O_NONBLOCK for the reader?
     *
     * If we use O_RDONLY (blocking), the reader hangs forever at open()
     * until a writer connects. With O_NONBLOCK, open() succeeds immediately
     * even without a writer. We then use poll() to wait for data — this
     * lets us respond to signals and do other work while waiting.
     *
     * This is the CORRECT pattern for daemon-style programs.
     */
    printf("[READER] Opening FIFO: %s (non-blocking mode)\n", FIFO_PATH);

    int fifo_fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (fifo_fd == -1) {
        perror("open fifo");
        return 1;
    }

    printf("[READER] FIFO opened (fd=%d). Waiting for data...\n", fifo_fd);
    printf("[READER] Press Ctrl+C to stop.\n\n");

    /* ---- Set up poll() ---- */
    struct pollfd fds[1];
    fds[0].fd      = fifo_fd;
    fds[0].events  = POLLIN;   /* We want to know when data arrives */
    fds[0].revents = 0;

    int total_messages = 0;
    int total_bytes    = 0;

    /* ---- Main poll loop ---- */
    while (g_running) {
        int ret = poll(fds, 1, POLL_TIMEOUT);

        if (ret == -1) {
            if (errno == EINTR) {
                /* Interrupted by signal — check g_running and continue */
                continue;
            }
            perror("poll");
            break;
        }

        if (ret == 0) {
            /* Timeout — no data arrived within POLL_TIMEOUT ms.
             * This is normal. We use the timeout so the loop
             * periodically checks g_running, allowing clean shutdown.
             * WITHOUT the timeout (-1), poll() blocks forever and
             * only signals can interrupt it. */
            continue;
        }

        /* ---- Check what events occurred ---- */

        /* POLLHUP: all writers closed the FIFO.
         * IMPORTANT: POLLHUP and POLLIN can fire simultaneously!
         * There may be unread data in the buffer. Read it first. */
        if (fds[0].revents & POLLIN) {
            char buf[READ_BUF_SIZE];

            /* Keep reading until the buffer is empty.
             * This handles the case where multiple messages
             * arrived between poll() calls. */
            while (1) {
                ssize_t n = read(fifo_fd, buf, sizeof(buf) - 1);
                if (n == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /* No more data available right now.
                         * This is NORMAL with O_NONBLOCK.
                         * Go back to poll() to wait for more. */
                        break;
                    }
                    perror("read");
                    g_running = 0;
                    break;
                }

                if (n == 0) {
                    /* EOF — all writers have closed the FIFO */
                    printf("\n[READER] FIFO closed by writer (EOF). "
                           "Total: %d messages, %d bytes.\n",
                           total_messages, total_bytes);
                    g_running = 0;
                    break;
                }

                /* Null-terminate for printing */
                buf[n] = '\0';

                /* Count messages (rough: count newlines) */
                for (ssize_t i = 0; i < n; i++) {
                    if (buf[i] == '\n')
                        total_messages++;
                }
                total_bytes += (int)n;

                /* Print received data */
                printf("%s", buf);
                fflush(stdout);  /* Ensure output is visible immediately */
            }
        }

        if (fds[0].revents & POLLHUP) {
            /* Hang-up detected. If we haven't already exited
             * (from read() returning 0), the writers disconnected
             * but there might still be data to read.
             * The next poll() will return POLLIN for remaining data,
             * and then read() will return 0. */
            if (!(fds[0].revents & POLLIN)) {
                printf("\n[READER] POLLHUP: All writers disconnected.\n");
                /* Don't break — a new writer might connect.
                 * For now, continue the poll loop. */
            }
        }

        if (fds[0].revents & POLLERR) {
            fprintf(stderr, "\n[READER] Error condition on FIFO.\n");
            break;
        }

        if (fds[0].revents & POLLNVAL) {
            fprintf(stderr, "\n[READER] Invalid file descriptor.\n");
            break;
        }

        /* Clear revents for next poll() call */
        fds[0].revents = 0;
    }

    /* ---- Cleanup ---- */
    printf("[READER] Shutting down. Total received: %d messages, "
           "%d bytes.\n", total_messages, total_bytes);

    close(fifo_fd);

    /* NOTE: We do NOT unlink(FIFO_PATH) here.
     * The FIFO is a persistent filesystem object.
     * It should survive the reader restarting.
     * Only delete it during uninstall/cleanup. */

    return 0;
}