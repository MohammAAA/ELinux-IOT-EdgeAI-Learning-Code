/*
 * 15_logd_enhanced.c — Multithreaded Logger Daemon (week 1 integration)
 *
 * The real problem in the previous code (14_logd.c): signal handlers are fundamentally incompatible with multithreaded programming,
 *                                        because they're asynchronous, global, and unpredictable.
 *                                        The solution is to eliminate them entirely.
 * The Solution: pthread_sigmask() + sigwait()
 * 
 * Why This Eliminates All The Problems:
 *                  Traditional Signal Handler
 *                    ════════════════════════════
 * • Executes asynchronously (interrupts ANY instruction)
 * • Runs in random thread context
 * • Can only call async-signal-safe functions (man 7 signal-safety) (No malloc, no printf, no mutex, ...)
 * • Reentrancy nightmare
 * • Race conditions with every shared variable
 *
 *                    sigwait() Approach
 *                    ═══════════════════
 * • Executes synchronously (like any other function call)
 * • Runs in a DEDICATED thread (predictable context)
 * • Can call ANY function (malloc, mutex, printf, free, etc.)
 * • No reentrancy issues
 * • No race conditions — it's just a normal function returning a value
 * • The signal becomes a normal event, like "data available on fd"
 * • This is NOT the same as "delivering" the signal.
 *   No signal handler fires. No async execution.
 *   sigwait() just polls the kernel's pending signal set.
 * 
 * 
 *  Signal Delivery Flow — Step by Step: (Here's what happens when you press Ctrl+C)
 *    You press Ctrl+C
 *         │
 *         ▼
 *    Terminal driver generates SIGINT
 *         │
 *         ▼
 *    Kernel: Which thread should receive SIGINT?
 *         │
 *         ├─ Main?    SIGINT is BLOCKED → skip
 *         ├─ Reader?  SIGINT is BLOCKED → skip
 *         ├─ Worker 0? SIGINT is BLOCKED → skip
 *         ├─ Worker 1? SIGINT is BLOCKED → skip
 *         └─ Worker 2? SIGINT is BLOCKED → skip
 *         │
 *         ▼
 *    Kernel: ALL threads have SIGINT blocked.
 *          Mark SIGINT as PENDING for the process.
 *         │
 *         ▼
 *    Signal thread is in sigwait({SIGINT, SIGTERM, SIGHUP}).
 *    Kernel checks: is SIGINT in sigwait's set? YES.
 *         │
 *         ▼
 *    sigwait() returns sig=SIGINT (synchronous, normal function return).
 *         │
 *         ▼
 *    signal_thread: case SIGINT: printf("SIGINT received"); goto shutdown;
 *         │
 *         ▼
 *    signal_thread: write(shutdown_pipe[1], "x", 1);
 *         │
 *         ▼
 *    reader_thread: poll() returns → sees shutdown pipe is readable
 *         │
 *         ▼
 *    reader_thread: break; → close(fd); → return NULL;
 *         │
 *         ▼
 *    signal_thread: rb_request_shutdown(&g_ring);
 *         │
 *         ▼
 *    Worker threads: rb_pop() sees shutdown + buffer empty → returns NULL → exit
 *         │
 *         ▼
 *    signal_thread: return NULL;
 *         │
 *         ▼
 *    main: pthread_join(sig_thread) returns ✓
 *    main: pthread_join(reader) returns ✓
 *    main: pthread_join(workers) returns ✓
 *    main: cleanup → exit(0)
 * 
 * 
 * 
 * 
 * 
 *

 * Build: gcc -Wall -Wextra -pthread -o ./bin/logd_enhanced 15_logd.c
 * Usage: ./bin/logd_enhanced [fifo_path] [log_path]
 *        ./bin/logd_enhanced                        # defaults: /tmp/logd.fifo /var/log/logd.log
 *
 * 
 * While testing, a new issue appears while trying to shutdown, it stucks again in the read_thread:
 *   "[signal] Shutdown initiated, returning from signal thread
 *   [logd] Worker 0 exiting
 *   [logd] Signal thread joined
 *   [logd] Worker 1 exiting
 *   [logd] Worker 2 exiting"
 * 
 * But the stuck now has different root cause:
 *      Timeline:
        ═══════════

        T1  We echo "hello" > /tmp/logd.fifo
        T2  poll() returns with POLLIN on FIFO (pfds[0])
        T3  Reader enters the INNER read() loop:

            while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
                // Read "hello" → n = 5 → process it
                // Loop back → read() again
                // read(fd, ...) ████ BLOCKS — waiting for MORE data
                //    (O_RDWR means no EOF, so read() blocks forever)
                //    The reader is STUCK HERE.
            }

        T4  We press Ctrl+C
        T5  Signal thread writes to shutdown pipe → "x"
        T6  But the reader is NOT in poll()!
            It's stuck in read(fd, ...) inside the inner loop.
            poll() never sees the shutdown pipe.
            ██████ HANG FOREVER ██████

 *       The shutdown pipe trick only works if the reader is inside poll().
 *       If the reader is inside read(fd, buf, ...) on the FIFO, the shutdown pipe write doesn't help — read() is blocking on a completely different file descriptor.
 *
 * 
 *  The solution is simple — set the FIFO fd to non-blocking mode after opening, and now the flow becomes:
 *      T1  You echo "hello" > /tmp/logd.fifo
 *      T2  poll() returns with POLLIN on FIFO
 *      T3  Reader enters inner read() loop:
 *          read() → "hello" (5 bytes) → process
 *          read() → -1, errno = EAGAIN → inner loop exits
 *      T4  Back to poll() — monitoring BOTH FIFO AND shutdown pipe ✓
 *      
 *      T5  You press Ctrl+C
 *      T6  Signal thread writes to shutdown pipe
 *      T7  poll() wakes up (shutdown pipe readable) → break → exit ✓
 *         
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

static ring_buffer_t         g_ring;       /* shared ring buffer */
static int                   g_log_fd = -1;/* log file descriptor */
/*
 * REMOVED: static volatile sig_atomic_t g_running = 1;
 *
 * Why? With the sigwait() approach, the signal handler thread
 * communicates shutdown via the shutdown pipe (for the reader)
 * and rb_request_shutdown() (for the workers). There's no need
 * for a shared flag that the main thread polls with sleep(1).
 *
 * The main thread now joins the signal thread directly.
 */
static int                   g_shutdown_pipe[2] = { -1, -1 };  /* The self (shutdown) pipe*/

/* ─── Signal Handling ───────────────────────────────────────────── */

/*
 * signal_thread_arg — passed to the signal handler thread
 *
 * Contains everything the signal thread needs to initiate shutdown.
 * Heap-allocated and freed by the thread itself (Rule #1 from Wednesday).
 */
typedef struct {
    int            shutdown_pipe_wr;  /* Write end of shutdown pipe */
    ring_buffer_t *ring;              /* Pointer to shared ring buffer */
    pthread_t      reader_tid;        /* Reader thread ID (for debugging) */
} signal_thread_arg_t;

/*
 * signal_thread — dedicated thread for synchronous signal handling
 *
 * This thread sits in sigwait() until SIGINT or SIGTERM arrives.
 * When it does, it initiates graceful shutdown:
 *   1. Writes to shutdown pipe → wakes reader's poll()
 *   2. Requests ring buffer shutdown → wakes workers' cond_wait()
 *
 * IMPORTANT: This is NOT a signal handler. It's a regular thread
 *            running a regular function. There are NO restrictions
 *            on what you can call here.
 *
 * We also handle SIGHUP for log rotation (reload log file):
 *   $ kill -HUP <pid>
 *   → Close and reopen the log file
 *   → Useful in production: allows log rotation without restarting
 */
static void *signal_thread(void *arg) {
    signal_thread_arg_t *sarg = (signal_thread_arg_t *)arg;
    int pipe_wr = sarg->shutdown_pipe_wr;
    ring_buffer_t *ring = sarg->ring;
    //pthread_t reader_tid = sarg->reader_tid;
    free(sarg);  /* Own this memory — free after copying fields */

    printf("[signal] Thread started (tid=%lu)\n",
           syscall(SYS_gettid));

    /* Set of signals we're waiting for */
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGINT);
    sigaddset(&wait_set, SIGTERM);
    sigaddset(&wait_set, SIGHUP);  /* Log rotation signal */

    while (1) {
        int sig;
        int ret = sigwait(&wait_set, &sig);

        if (ret != 0) {
            errno = ret;
            perror("sigwait");
            continue;  /* Keep running — don't crash */
        }

        switch (sig) {
        case SIGINT:
            printf("\n[signal] SIGINT received (Ctrl+C)\n");
            goto shutdown;

        case SIGTERM:
            printf("\n[signal] SIGTERM received\n");
            goto shutdown;

        case SIGHUP:
            printf("[signal] SIGHUP received — rotating log file\n");
            /*
             * Log rotation: close and reopen the log file.
             *
             * In production, you'd:
             *   1. Close current fd
             *   2. Rename current log to log.1, log.1 to log.2, etc.
             *   3. Open new log file
             *   4. Update g_log_fd (atomically — or use a mutex)
             *
             * For now, just close and reopen. This is safe because
             * workers use their own local copy of the fd (from warg->log_fd).
             *
             * TODO: Make g_log_fd a pointer shared via mutex so workers
             *       always use the current fd after rotation.
             */
            close(g_log_fd);
            g_log_fd = open(LOG_PATH,
                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (g_log_fd < 0) {
                perror("[signal] reopen log file after SIGHUP");
            }
            break;

        default:
            printf("[signal] Unexpected signal %d (%s)\n",
                   sig, strsignal(sig));
            break;
        }
    }

shutdown:
    /* Write to shutdown pipe — wakes reader's poll() immediately */
    if (pipe_wr >= 0) {
        ssize_t n = write(pipe_wr, "x", 1);
        if (n < 0 && errno != EPIPE) {
            perror("[signal] write to shutdown pipe");
        }
    }

    /* Request ring buffer shutdown — wakes all workers */
    rb_request_shutdown(ring);

    printf("[signal] Shutdown initiated, returning from signal thread\n");
    return NULL;
}
/*
 * setup_signals — configure signal handling for the entire process
 *
 * This MUST be called BEFORE any pthread_create().
 * The signal mask is inherited by all new threads.
 *
 * Changes from previous version:
 *   - NO signal handler function (no more signal_handler())
 *   - Block SIGINT/SIGTERM instead of catching them
 *   - Ignore SIGPIPE (unchanged)
 *   - No SA_RESTART concerns (signals are blocked, not caught)
 */
static void setup_signals(void) {
    /*
     * Step 1: Ignore SIGPIPE.
     *
     * We ignore (not block) SIGPIPE because:
     *   - Blocked signals become pending and must be handled eventually
     *   - Ignored signals are simply discarded
     *   - We never want SIGPIPE, ever; discard is correct
     */
    struct sigaction sa_ignore;
    memset(&sa_ignore, 0, sizeof(sa_ignore));
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sigaction(SIGPIPE, &sa_ignore, NULL);

    /*
     * Step 2: Block SIGINT, SIGTERM, and SIGHUP.
     *
     * These signals are now BLOCKED (not ignored).
     * They will be PENDING until the signal thread calls sigwait().
     *
     */
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGINT);
    sigaddset(&block_set, SIGTERM);
    sigaddset(&block_set, SIGHUP);

    /*
     * pthread_sigmask — NOT sigprocmask!
     *
     * From POSIX: "The behavior of sigprocmask() is undefined
     * in a multithreaded process." Use pthread_sigmask() always
     * when threads exist or will exist.
     *
     * SIG_BLOCK: adds to current mask (doesn't remove existing blocks)
     */
    if (pthread_sigmask(SIG_BLOCK, &block_set, NULL) != 0) {
        perror("pthread_sigmask");
        exit(1);  /* Fatal — can't run safely without signal control */
    }

    printf("[signal] SIGINT, SIGTERM, SIGHUP blocked in main thread\n");
    printf("[signal] All future threads will inherit this mask\n");
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
 * Changes from previous code:
 *   - Removed g_running check (signal thread handles shutdown)
 *   - Shutdown pipe is the sole shutdown mechanism
 *   - Removed POLLHUP handling (O_RDWR + shutdown pipe approach)
 *   - The critical change: poll(pfds, nfds, -1) now uses infinite timeout.
 *     We no longer need a periodic timeout to check g_running because the shutdown pipe wakes poll() immediately when the signal thread writes to it.
 */
static void *reader_thread(void *arg) {
    (void)arg;
    int fd = -1;
    char buf[4096];
    struct pollfd pfds[2];
    int nfds;

    printf("[logd] Reader thread started, opening FIFO...\n");

    fd = open(FIFO_PATH, O_RDWR);
    if (fd < 0) {
        perror("open FIFO");
        /* Write to shutdown pipe so main thread doesn't wait forever */
        if (g_shutdown_pipe[1] >= 0)
            write(g_shutdown_pipe[1], "x", 1);
        rb_request_shutdown(&g_ring);
        return NULL;
    }

    /* Make FIFO non-blocking.
     *
     * Why: The inner read() loop drains all available data. When the
     * pipe buffer is empty, read() with O_NONBLOCK returns -1/EAGAIN
     * instead of blocking. This lets us return to poll(), which
     * also monitors the shutdown pipe.
     *
     * Without this, read() blocks indefinitely inside the loop
     * when no data is available (because O_RDWR means no EOF),
     * and we never check the shutdown pipe.
     */
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        close(fd);
        if (g_shutdown_pipe[1] >= 0)
            write(g_shutdown_pipe[1], "x", 1);
        rb_request_shutdown(&g_ring);
        return NULL;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL O_NONBLOCK");
        close(fd);
        if (g_shutdown_pipe[1] >= 0)
            write(g_shutdown_pipe[1], "x", 1);
        rb_request_shutdown(&g_ring);
        return NULL;
    }

    printf("[logd] FIFO opened (non-blocking), reading messages...\n");

    while (1) {  /* No g_running flag — exit only via shutdown pipe */
        memset(pfds, 0, sizeof(pfds));

        pfds[0].fd = fd;
        pfds[0].events = POLLIN;

        if (g_shutdown_pipe[0] >= 0) {
            pfds[1].fd = g_shutdown_pipe[0];
            pfds[1].events = POLLIN;
            nfds = 2;
        } else {
            nfds = 1;
        }

        int ret = poll(pfds, nfds, -1);  /* -1 = block forever (no timeout needed!) */

        if (ret < 0) {
            if (errno == EINTR) {
                /*
                 * EINTR on a blocked signal?
                 * This shouldn't happen — SIGINT/SIGTERM are blocked.
                 * But if an unblocked signal (like SIGSEGV handler, or
                 * a profiling timer signal) interrupts poll, handle it.
                 */
                continue;
            }
            perror("poll");
            break;
        }

        if (ret == 0) {
            /* With -1 timeout, this should never happen */
            continue;
        }

        /* Shutdown pipe readable → time to exit */
        if (nfds == 2 && (pfds[1].revents & POLLIN)) {
            char dummy;
            while (read(g_shutdown_pipe[0], &dummy, 1) > 0)
                ;  /* Drain all bytes */
            printf("[logd] Reader received shutdown signal via pipe\n");
            break;
        }

        /* FIFO has data — drain it with non-blocking reads */
        if (pfds[0].revents & POLLIN) {
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                if (n > 0 && buf[n - 1] == '\n') {
                    buf[n - 1] = '\0';
                }

                char *msg_copy = strdup(buf);
                if (msg_copy == NULL) {
                    perror("strdup");
                    continue;
                }

                if (rb_push(&g_ring, msg_copy) != 0) {
                    free(msg_copy);
                    break; /* Ring buffer shutdown — exit inner loop */
                }
            }
            /* Inner loop exits here because:
             *   - n <= 0 (no more data)
             *   - n == -1, errno == EAGAIN (normal — pipe buffer empty)
             *   - n == -1, other errno (error)
             *   - rb_push returned -1 (shutdown)
             *
             * In ALL cases, we fall through to the top of the while(1)
             * loop and go back to poll() — which checks the shutdown pipe.
             */
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read FIFO");
                break;  /* Actual error — exit outer loop */
            }
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

    /*
     * Step 4: Create shutdown pipe — BEFORE signal setup and thread creation.
     *
     * The signal thread needs this pipe to notify the reader.
     */
    if (pipe(g_shutdown_pipe) < 0) {
        perror("pipe");
        rb_destroy(&g_ring);
        close(g_log_fd);
        return 1;
    }

    /*
     * Step 5: Block signals BEFORE creating ANY thread.
     *
     * This is the MOST CRITICAL ordering requirement.
     * After this call, SIGINT/SIGTERM/SIGHUP are blocked in the
     * main thread. Every thread created after this inherits
     * the same blocked mask.
     *
     * The signal handler thread (created next) will use sigwait()
     * to synchronously receive these signals.
     */
    setup_signals();

    /*
     * Step 6: Create signal handler thread FIRST.
     *
     * Why first?
     *   - If SIGINT arrives between creating the reader and the
     *     signal thread, we need the signal thread to be ready.
     *   - Creating it first minimizes the unprotected window.
     *
     * Note: the signal thread inherits the blocked mask from main.
     *       sigwait() works BECAUSE the signals are blocked.
     */
    pthread_t sig_thread;
    {
        signal_thread_arg_t *sarg = malloc(sizeof(signal_thread_arg_t));
        if (!sarg) {
            perror("malloc signal_thread_arg_t");
            close(g_shutdown_pipe[0]);
            close(g_shutdown_pipe[1]);
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
        sarg->shutdown_pipe_wr = g_shutdown_pipe[1];
        sarg->ring = &g_ring;
        /* reader_tid is 0 for now — we don't know it yet.
         * In a production system, you'd pass it after creating the reader.
         * For now it's only used for debugging, so 0 is fine. */

        int ret = pthread_create(&sig_thread, NULL, signal_thread, sarg);
        if (ret != 0) {
            fprintf(stderr, "pthread_create signal_thread: %s\n",
                    strerror(ret));
            free(sarg);
            close(g_shutdown_pipe[0]);
            close(g_shutdown_pipe[1]);
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
    }

    /* Step 7: Launch worker (consumer) threads */
    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_arg_t *warg = malloc(sizeof(worker_arg_t));
        if (!warg) {
            perror("malloc worker_arg_t");
            rb_request_shutdown(&g_ring);
            pthread_join(sig_thread, NULL);
            for (int j = 0; j < i; j++) pthread_join(workers[j], NULL);
            close(g_shutdown_pipe[0]);
            close(g_shutdown_pipe[1]);
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
        warg->id = i;
        warg->log_fd = g_log_fd;

        int ret = pthread_create(&workers[i], NULL, worker_thread, warg);
        if (ret != 0) {
            fprintf(stderr, "pthread_create worker %d: %s\n",
                    i, strerror(ret));
            free(warg);
            rb_request_shutdown(&g_ring);
            pthread_join(sig_thread, NULL);
            for (int j = 0; j < i; j++) pthread_join(workers[j], NULL);
            close(g_shutdown_pipe[0]);
            close(g_shutdown_pipe[1]);
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
    }

    /* Step 8: Launch reader (producer) thread */
    pthread_t reader;
    {
        int ret = pthread_create(&reader, NULL, reader_thread, NULL);
        if (ret != 0) {
            fprintf(stderr, "pthread_create reader: %s\n", strerror(ret));
            rb_request_shutdown(&g_ring);
            pthread_join(sig_thread, NULL);
            for (int i = 0; i < NUM_WORKERS; i++)
                pthread_join(workers[i], NULL);
            close(g_shutdown_pipe[0]);
            close(g_shutdown_pipe[1]);
            rb_destroy(&g_ring);
            close(g_log_fd);
            return 1;
        }
    }

    printf("[logd] All threads launched. Running.\n");
    printf("[logd] Signal handling: synchronous via dedicated thread\n");
    printf("[logd] Send SIGTERM or press Ctrl+C to stop.\n");
    printf("[logd] Send SIGHUP (kill -HUP %d) to rotate log file.\n", getpid());

    /*
     * Step 9: Main thread waits for signal thread to finish.
     *
     * The signal thread only returns after handling SIGINT/SIGTERM.
     * When it returns, it has already:
     *   - Written to the shutdown pipe (reader will exit)
     *   - Called rb_request_shutdown (workers will drain and exit)
     *
     * So we just join in order: signal → reader → workers.
     */
    pthread_join(sig_thread, NULL);
    printf("[logd] Signal thread joined\n");

    pthread_join(reader, NULL);
    printf("[logd] Reader thread joined\n");

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }
    printf("[logd] All workers joined\n");

    /* Step 10: Cleanup */
    rb_destroy(&g_ring);
    close(g_log_fd);
    close(g_shutdown_pipe[0]);
    close(g_shutdown_pipe[1]);

    printf("[logd] Clean shutdown complete.\n");
    return 0;
}