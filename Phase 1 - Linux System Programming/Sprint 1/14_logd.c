/*
 * 14_logd.c — Multithreaded Logger Daemon (week 1 integration)
 *
 * Architecture:
 *   [External Writers] → FIFO → [Reader Thread] → [Ring Buffer]
 *                                                         ↓
 *                                            [Worker Threads (N)]
 *                                                         ↓
 *                                                  [Log File]
 *
 * Build: gcc -Wall -Wextra -pthread -o ./bin/logd 14_logd.c
 * Usage: ./bin/logd [fifo_path] [log_path]
 *        ./bin/logd                        # defaults: /tmp/logd.fifo /var/log/logd.log
 
    Test:
        Terminal 1: Start the daemon ($ ./bin/logd)
        Terminal 2: Send messages (# Simple message
                                    $ echo "Hello from terminal" > /tmp/logd.fifo

                                    # Multiple messages (the daemon will survive the disconnect!)
                                    $ echo "Message 1" > /tmp/logd.fifo
                                    $ echo "Message 2" > /tmp/logd.fifo
                                    $ echo "Critical alert!" > /tmp/logd.fifo

                                    # Rapid fire from a script
                                    $ for i in $(seq 1 100); do echo "Load test message $i" > /tmp/logd.fifo; done)
                                    )
        Terminal 3: Watch the log file ($ tail -f /var/log/logd.log)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <pthread.h>
#include <sys/syscall.h> /* For SYS_gettid */

/* ─── Configuration ─────────────────────────────────────────────── */
#define FIFO_PATH       "/tmp/logd.fifo" /* the /tmp/ folder contents are stored in RAM (via the tmpfs)*/
#define LOG_PATH        "/var/log/logd.log" /* the /var/ folder contents are stored on disk (slow IO), it is used for growing, persistent, and variable files that are actively modified by the system while it is running. This includes system logs, databases, ...*/
#define RING_BUF_SIZE   4096
#define NUM_WORKERS     3
#define POLL_TIMEOUT_MS 5000     /* 5 seconds — reader checks for shutdown */

/* ─── Ring Buffer ───────────────────────────────────────────────── */

typedef struct {
    char           *messages[RING_BUF_SIZE];
    int             head;           /* dequeue from here */
    int             tail;           /* enqueue here */
    int             count;          /* current ring buffer allocation count */
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;      /* consumers wait on this */
    pthread_cond_t  not_full;       /* producers wait on this */
    int             shutdown;       /* 0 = running, 1 = shutting down */
} ring_buffer_t;

/*
 * rb_init — initialize ring buffer to empty state
 *
 * Why PTHREAD_MUTEX_INITIALIZER / PTHREAD_COND_INITIALIZER?
 *   Static initialization is preferred when possible because:
 *   1. No runtime overhead (happens at compile/load time)
 *   2. No error checking needed (can't fail)
 *   3. No need to call destroy() if the object has static storage
 *      duration (global or static local)
 *
 *   We use dynamic init here because the ring_buffer_t is allocated
 *   on the heap via calloc(), so static initializers won't work.
 */
static int rb_init(ring_buffer_t *rb) {
    memset(rb, 0, sizeof(*rb));
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->shutdown = 0;

    int ret;
    ret = pthread_mutex_init(&rb->mutex, NULL);
    if (ret != 0) {
        errno = ret;
        perror("pthread_mutex_init");
        return -1;
    }
    ret = pthread_cond_init(&rb->not_empty, NULL);
    if (ret != 0) {
        errno = ret;
        perror("pthread_cond_init not_empty");
        pthread_mutex_destroy(&rb->mutex);
        return -1;
    }
    ret = pthread_cond_init(&rb->not_full, NULL);
    if (ret != 0) {
        errno = ret;
        perror("pthread_cond_init not_full");
        pthread_mutex_destroy(&rb->mutex);
        pthread_cond_destroy(&rb->not_empty);
        return -1;
    }
    return 0;
}

/*
 * rb_destroy — free all remaining messages and destroy sync primitives
 *
 * IMPORTANT: Only call after all producer/consumer threads have stopped.
 *           Otherwise we'll destroy a mutex that a thread is blocked on.
 */
static void rb_destroy(ring_buffer_t *rb) {
    /* Free any messages still in the buffer (e.g., during shutdown) */
    for (int i = 0; i < rb->count; i++) {
        int idx = (rb->head + i) % RING_BUF_SIZE; /* We need to mod RING_BUF_SIZE to handle the cases when the buffer wraps on itself*/
        free(rb->messages[idx]);
    }
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
}

/*
 * rb_push — enqueue a message (called by producer: reader thread)
 *
 * Blocks if the buffer is full. Returns -1 if shutdown was requested.
 *
 * The caller passes ownership of 'msg' to the ring buffer.
 * The ring buffer will free(msg) when the message is consumed or
 * when the buffer is destroyed.
 */
static int rb_push(ring_buffer_t *rb, char *msg) {
    pthread_mutex_lock(&rb->mutex);

    /* Wait until there's space, or we're shutting down.
     *
     * Why 'while' not 'if'?
     *   1. Spurious wakeups — POSIX allows cond_wait to return without signal
     *   2. After wakeup, another thread may have filled the slot first
     *      (multiple producers racing)
     */
    while (rb->count == RING_BUF_SIZE && !rb->shutdown) {
        pthread_cond_wait(&rb->not_full, &rb->mutex);
    }

    if (rb->shutdown) {
        pthread_mutex_unlock(&rb->mutex);
        return -1;  /* Caller must free(msg) */
    }

    rb->messages[rb->tail] = msg;
    rb->tail = (rb->tail + 1) % RING_BUF_SIZE;
    rb->count++;

    /* Signal ONE waiting consumer.
     * Use signal (not broadcast) because we only added ONE item.
     * If there are 5 consumers waiting, only one can consume this item.
     * Waking all 5 would cause 4 unnecessary wakeups → thundering herd.
     */
    pthread_cond_signal(&rb->not_empty);

    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

/*
 * rb_pop — dequeue a message (called by consumer: worker threads)
 *
 * Blocks if the buffer is empty. Returns NULL if shutdown + buffer empty.
 *
 * The caller receives ownership of the returned string and must free() it.
 */
static char *rb_pop(ring_buffer_t *rb) {
    pthread_mutex_lock(&rb->mutex);

    /* Wait until there's data, or we're shutting down.
     *
     * The condition is: empty AND not shutting down
     * When shutdown is requested AND buffer is empty, we return NULL
     * to tell the worker to exit.
     */
    while (rb->count == 0 && !rb->shutdown) {
        pthread_cond_wait(&rb->not_empty, &rb->mutex);
    }

    /* Buffer empty + shutdown = time to exit */
    if (rb->count == 0 && rb->shutdown) {
        pthread_mutex_unlock(&rb->mutex);
        return NULL;
    }

    char *msg = rb->messages[rb->head];
    rb->messages[rb->head] = NULL;  /* Prevent dangling pointer */
    rb->head = (rb->head + 1) % RING_BUF_SIZE;
    rb->count--;

    /* Signal ONE waiting producer — we freed only one slot */
    pthread_cond_signal(&rb->not_full);

    pthread_mutex_unlock(&rb->mutex);
    return msg;
}

/*
 * rb_request_shutdown — signal all threads to stop
 *
 * Uses broadcast because ALL waiting threads need to wake up:
 *   - Workers waiting on not_empty need to drain remaining items and exit
 *   - The reader waiting on not_full needs to stop pushing
 */
static void rb_request_shutdown(ring_buffer_t *rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->shutdown = 1;
    /* Wake EVERYONE — both producers and consumers */
    pthread_cond_broadcast(&rb->not_empty);
    pthread_cond_broadcast(&rb->not_full);
    pthread_mutex_unlock(&rb->mutex);
}

/* ─── Global State ──────────────────────────────────────────────── */

static ring_buffer_t g_ring;       /* shared ring buffer */
static int           g_log_fd = -1;/* log file descriptor */
static volatile sig_atomic_t g_running = 1;  /* set to 0 by signal handler */

/* ─── Signal Handling ───────────────────────────────────────────── */

/*
 * signal_handler — minimal async-signal-safe shutdown trigger
 *
 * CRITICAL RULES for signal handlers:
 *   ✓ Only call async-signal-safe functions (see man 7 signal-safety)
 *   ✓ Only modify variables of type volatile sig_atomic_t
 *   ✗ NEVER call: printf, malloc, free, pthread_mutex_lock, fprintf, etc.
 *   ✗ NEVER do I/O inside a signal handler
 *
 * We just set a flag. The main thread's poll() loop detects
 * g_running == 0 and initiates clean shutdown.
 */
static void signal_handler(int sig) {
    (void)sig;  /* Suppress unused parameter warning */
    g_running = 0;
}

/*
 * setup_signals — block SIGTERM/SIGINT in main thread, set up handler
 *
 * Why sigaction() not signal()?
 *   signal() has different semantics across UNIX implementations.
 *   sigaction() is POSIX-standard and reliable.
 *
 * We DON'T mask signals here in the main thread because the reader thread
 * calls poll() with a timeout — it will notice g_running == 0 within
 * POLL_TIMEOUT_MS. Tomorrow's lesson will cover proper per-thread masking.
 */
static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    /* SA_RESTART: automatically restart interrupted syscalls (read, write, poll)
     * Without this, poll() returns EINTR on signal, which we'd have to handle.
     * With SA_RESTART, poll() restarts its wait.
     * But we DON'T want this for poll — we want poll to return so we can check g_running.
     * So we explicitly DON'T set SA_RESTART. */
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Ignore SIGPIPE — if a writer disconnects, we don't want to die */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/* ─── Timestamp ─────────────────────────────────────────────────── */

/*
 * get_timestamp — returns ISO 8601 timestamp string
 *
 * Format: "2026-05-17T14:30:45.123Z"
 *
 * Returns pointer to static buffer (NOT thread-safe).
 * This is fine because only worker threads call it, and each call
 * happens within the worker's own execution context — but if two workers
 * call it simultaneously, they'd overwrite each other's buffer.
 *
 * TODO (for you): Make this thread-safe with thread-local storage or
 *                 pass a caller-supplied buffer.
 */
static const char *get_timestamp(void) {
    static char buf[64];
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);  /* _r variant is thread-safe */

    int len = snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ld",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        ts.tv_nsec / 1000000L);

    buf[len] = '\0';
    return buf;
}

/* ─── Worker Thread ─────────────────────────────────────────────── */

/*
 * Worker argument — passed via heap allocation
 */
typedef struct {
    int    id;      /* Worker number: 0, 1, 2, ... */
    int    log_fd;  /* Log file descriptor */
} worker_arg_t;

/*
 * worker_thread — consumer: pops messages from ring buffer, writes to log
 *
 * Format per line:
 *   [TIMESTAMP] [W<id>] <message>
 *
 * Example:
 *   [2026-05-17T14:30:45.123Z] [W0] User login from 192.168.1.10
 */
static void *worker_thread(void *arg) {
    worker_arg_t *warg = (worker_arg_t *)arg;
    int id = warg->id;
    int fd = warg->log_fd;
    free(warg);  /* We own this memory — free immediately after copying fields */

    printf("[logd] Worker %d started (tid=%ld)\n", id,
           syscall(SYS_gettid));

    char *msg;
    while ((msg = rb_pop(&g_ring)) != NULL) {
        /*
         * Format the log line.
         *
         * WARNING: get_timestamp() uses a static buffer.
         * If two workers call this simultaneously, they'll race on the buffer.
         * For now this is acceptable — the worst case is a slightly garbled
         * timestamp. Next exercise: fix with snprintf into a local buffer.
         */
        const char *ts = get_timestamp();

        /* Build: [timestamp] [W<id>] <msg>\n */
        char linebuf[4096];
        int linelen = snprintf(linebuf, sizeof(linebuf),
            "[%s] [W%d] %s\n", ts, id, msg);

        free(msg);  /* We own this string — free after use */

        if (linelen < 0 || (size_t)linelen >= sizeof(linebuf)) {
            /* Message too long for buffer — truncate */
            linelen = sizeof(linebuf) - 2;
            linebuf[linelen] = '\n';
            linebuf[linelen + 1] = '\0';
            linelen++;
        }

        /* Write to log file — handle partial writes + EINTR */
        ssize_t total_written = 0;
        while (total_written < linelen) {
            ssize_t n = write(fd, linebuf + total_written,
                              linelen - total_written);
            if (n < 0) {
                if (errno == EINTR)
                {
                    continue;  /* Interrupted by signal — retry */
                }
                perror("write to log file");
                break;
            }
            total_written += n;
        }
    }

    printf("[logd] Worker %d exiting\n", id);
    return NULL;
}

/* ─── FIFO Reader Thread ────────────────────────────────────────── */

/*
 * reader_thread — producer: reads from FIFO, pushes into ring buffer
 *
 * Uses poll() with a timeout so it can periodically check g_running
 * even when no data arrives.
 *
 * Buffer for reading: 4096 bytes — matches PIPE_BUF on Linux.
 * This means a single write() of <= 4096 bytes to the FIFO is atomic.
 * We won't get partial messages from a single writer.
 */
static void *reader_thread(void *arg) {
    (void)arg;  /* No data needed */
    int fd = -1;
    char buf[4096];
    struct pollfd pfd;

    printf("[logd] Reader thread started, opening FIFO...\n");

    /*
     * Open the FIFO — BLOCKS until a writer connects.
     *
     * This is the first thing the reader does. The daemon appears to
     * "hang" here until someone opens the FIFO for writing.
     * That's correct behavior — the daemon is ready and waiting.
     */
    fd = open(FIFO_PATH, O_RDONLY);
    if (fd < 0) {
        perror("open FIFO");
        g_running = 0;  /* Signal shutdown to main thread */
        return NULL;
    }

    printf("[logd] FIFO opened, reading messages...\n");

    while (g_running) {
        pfd.fd = fd;
        pfd.events = POLLIN;     /* We want to know when data is available */
        pfd.revents = 0;

        /*
         * poll() with timeout — why not block forever?
         *
         * If we block forever in poll(), we'd never notice g_running == 0
         * (set by the signal handler) until new data arrives.
         * The timeout ensures we check periodically.
         *
         * POLL_TIMEOUT_MS = 5000 (5 seconds) is a good balance:
         *   - Short enough: daemon responds to SIGTERM within 5s
         *   - Long enough: minimal unnecessary wakeups (zero CPU while sleeping)
         */
        int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (ret < 0) {
            if (errno == EINTR) {
                /* Interrupted by signal — check g_running and loop */
                continue;
            }
            perror("poll");
            break;
        }

        if (ret == 0) {
            /* Timeout — no data. Check shutdown flag and continue. */
            continue;
        }

        /*
         * POLLIN: data available to read
         *
         * Read in a loop until EAGAIN/EWOULDBLOCK (no more data in pipe buffer)
         * This drains ALL available data, not just one message.
         */
        if (pfd.revents & POLLIN) {
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';  /* Null-terminate for string handling */

                /* Strip trailing newline if present */
                if (n > 0 && buf[n - 1] == '\n') {
                    buf[n - 1] = '\0';
                }

                /* Duplicate the message for the ring buffer.
                 * rb_push() takes ownership — it will free() this. */
                char *msg_copy = strdup(buf);
                if (msg_copy == NULL) {
                    perror("strdup");
                    continue;  /* Drop message — don't crash */
                }

                if (rb_push(&g_ring, msg_copy) != 0) {
                    /* Shutdown requested — free the message ourselves */
                    free(msg_copy);
                    break;
                }
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read FIFO");
            }
        }

        /*
         * POLLHUP: writer disconnected.
         *
         * CRITICAL EDGE CASE (from Tuesday lesson!):
         * POLLHUP can arrive WITH POLLIN.
         * If there's still data in the pipe buffer, we must drain it first.
         *
         * The read loop above already drained available data.
         * After POLLHUP, we reopen the FIFO to accept new writers.
         *
         * This is what makes logd a proper daemon — it survives
         * writer disconnects and reconnects.
         */
        if (pfd.revents & POLLHUP) {
            printf("[logd] Writer disconnected, reopening FIFO...\n");
            close(fd);
            fd = open(FIFO_PATH, O_RDONLY);
            if (fd < 0) {
                if (g_running) {
                    perror("reopen FIFO");
                }
                break;
            }
            printf("[logd] FIFO reopened, waiting for writers...\n");
        }
    }

    close(fd);
    printf("[logd] Reader thread exiting\n");
    return NULL;
}

/* ─── Daemon Setup ──────────────────────────────────────────────── */

/*
 * create_fifo — create the FIFO if it doesn't exist
 *
 * mkfifo() creates a named pipe filesystem entry.
 * It fails with EEXIST if the file already exists — that's fine.
 * Any other error is fatal.
 */
static int create_fifo(const char *path) {
    if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo");
        return -1;
    }
    /* Ensure correct permissions even if file existed */
    chmod(path, 0666);
    return 0;
}

/*
 * open_log_file — open (or create) the log file with append mode
 *
 * O_APPEND: every write goes to the end, even if another process
 *           also has the file open (atomic on Linux for <= PIPE_BUF).
 * O_CREAT:  create if doesn't exist.
 * O_CLOEXEC: close on exec — prevents FD leaks if we ever exec().
 */
static int open_log_file(const char *path) {
    int fd = open(path,
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
        0644);  /* -rw-r--r-- */

    if (fd < 0) {
        perror("open log file");
        return -1;
    }
    return fd;
}

/* ─── Main ──────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *fifo_path = (argc > 1) ? argv[1] : FIFO_PATH;
    const char *log_path  = (argc > 2) ? argv[2] : LOG_PATH;

    printf("=== logd — Multithreaded Logger Daemon ===\n");
    printf("FIFO: %s\n", fifo_path);
    printf("Log:  %s\n", log_path);
    printf("Workers: %d\n", NUM_WORKERS);

    /* Step 1: Create the FIFO */
    if (create_fifo(fifo_path) < 0) {
        fprintf(stderr, "Failed to create FIFO at %s\n", fifo_path);
        return 1;
    }

    /* Step 2: Open the log file */
    g_log_fd = open_log_file(log_path);
    if (g_log_fd < 0) {
        fprintf(stderr, "Failed to open log file at %s\n", log_path);
        return 1;
    }

    /* Step 3: Initialize the ring buffer */
    if (rb_init(&g_ring) < 0) {
        fprintf(stderr, "Failed to initialize ring buffer\n");
        close(g_log_fd);
        return 1;
    }

    /* Step 4: Set up signal handlers */
    setup_signals();

    /* Step 5: Launch worker (consumer) threads */
    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_arg_t *warg = malloc(sizeof(worker_arg_t));
        if (!warg) {
            perror("malloc worker_arg_t");
            /* Shut down everything — future: graceful degradation */
            rb_request_shutdown(&g_ring);
            for (int j = 0; j < i; j++) {
                pthread_join(workers[j], NULL);
            }
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
        warg->id = i;
        warg->log_fd = g_log_fd;

        int ret = pthread_create(&workers[i], NULL, worker_thread, warg);
        if (ret != 0) {
            fprintf(stderr, "pthread_create worker %d: %s\n", i, strerror(ret));
            free(warg);
            /* Clean up already-created threads */
            rb_request_shutdown(&g_ring);
            for (int j = 0; j < i; j++) {
                pthread_join(workers[j], NULL);
            }
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
    }

    /* Step 6: Launch reader (producer) thread */
    pthread_t reader;
    {
        int ret = pthread_create(&reader, NULL, reader_thread, NULL);
        if (ret != 0) {
            fprintf(stderr, "pthread_create reader: %s\n", strerror(ret));
            rb_request_shutdown(&g_ring);
            for (int i = 0; i < NUM_WORKERS; i++) {
                pthread_join(workers[i], NULL);
            }
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
    }

    printf("[logd] All threads launched. Running. (Ctrl+C or SIGTERM to stop)\n");

    /* Step 7: Main thread waits for shutdown signal.
     *
     * The main thread's job after launching threads is simple:
     * wait for g_running to become 0 (set by signal handler).
     *
     * We use a sleep loop instead of pause() because:
     *   pause() only returns on signal delivery, and SA_RESTART makes
     *   it not return at all (it restarts).
     *   A sleep loop is simpler and more portable.
     */
    while (g_running) {
        sleep(1);
    }

    printf("\n[logd] Shutdown initiated...\n");

    /* Step 8: Graceful shutdown sequence
     *
     * Order matters:
     *   1. Stop the reader (no new messages)
     *   2. Tell ring buffer to shut down (wake all workers)
     *   3. Wait for reader thread
     *   4. Wait for all worker threads (they drain remaining messages)
     *   5. Destroy ring buffer (frees any remaining messages)
     *   6. Close log file
     *   7. Remove FIFO (optional — some daemons leave it)
     */

    /* Request shutdown — this wakes all threads blocked on cond_wait */
    rb_request_shutdown(&g_ring);

    /* Wait for reader to finish */
    pthread_join(reader, NULL);
    printf("[logd] Reader thread joined\n");

    /* Wait for all workers to finish (they drain remaining messages) */
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }
    printf("[logd] All workers joined\n");

    /* Cleanup */
    rb_destroy(&g_ring);
    close(g_log_fd);

    printf("[logd] Clean shutdown complete.\n");
    return 0;
}