/*
 * 11_thread_pool.c — A simple fixed-size thread pool
 *
 * Features:
 *   - 4 worker threads (configurable)
 *   - Work queue with FIFO ordering
 *   - Graceful shutdown
 *   - Per-job function pointer and argument
 *
 * NOTE: This version intentionally does NOT use mutexes on the work queue
 *       to demonstrate race conditions. Tomorrow we'll fix this with
 *       mutex + condition variables.
 *
 * Compile: gcc -Wall -Wextra -pthread -o bin/thread_pool 11_thread_pool.c
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE /* Must be at the very top */  
#include <unistd.h> /* For usleep and syscall */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/syscall.h> /* For SYS_gettid */
#include <sys/types.h> /* For pid_t, if needed */

/* ============================================================
 * Configuration
 * ============================================================ */
#define THREAD_POOL_SIZE   4
#define WORK_QUEUE_CAPACITY 1024

/* ============================================================
 * Work Item — a unit of work submitted to the pool
 * ============================================================ */
typedef struct {
    void (*function)(void *arg);  /* Function to execute */
    void *arg;                    /* Argument for the function */
    int   job_id;                 /* For debugging/logging */
} work_item_t;

/* ============================================================
 * Work Queue — circular buffer of work items
 * ============================================================ */
typedef struct {
    work_item_t items[WORK_QUEUE_CAPACITY];
    int         head;     /* Next item to dequeue (read by workers) */
    int         tail;     /* Next slot to enqueue (written by submitter) */
    int         count;    /* Current number of items in queue */
    int         total_submitted;
    int         total_completed;
} work_queue_t;

/* ============================================================
 * Thread Pool
 * ============================================================ */
typedef struct {
    pthread_t    threads[THREAD_POOL_SIZE];
    work_queue_t queue;
    int          shutdown_requested;  /* Flag to tell workers to stop */
} thread_pool_t;

/* Global pool — shared by all threads */
static thread_pool_t g_pool;

/* ============================================================
 * Worker function — runs in each thread
 * ============================================================ */
static void *pool_worker(void *arg)
{
    long thread_id = (long)arg;

    printf("  [Worker %ld] Started\n", thread_id);

    while (!g_pool.shutdown_requested) {

        /* Check if there's work to do.
         * NOTE: This read of queue.head, queue.tail, queue.count
         * is a RACE CONDITION without a mutex!
         * Tomorrow we'll fix this properly.
         * For today, we're observing the race. */
        if (g_pool.queue.count > 0) {

            /* Dequeue a work item */
            work_item_t item = g_pool.queue.items[g_pool.queue.head];
            g_pool.queue.head = (g_pool.queue.head + 1) % WORK_QUEUE_CAPACITY;
            g_pool.queue.count--;

            /* Execute the work */
            item.function(item.arg);

            /* Update completion counter */
            g_pool.queue.total_completed++;

        } else {
            /* No work — yield the CPU to other threads */
            sched_yield();
        }
    }

    printf("  [Worker %ld] Shutting down (completed %d jobs total)\n",
           thread_id, g_pool.queue.total_completed);
    return NULL;
}

/* ============================================================
 * Thread pool initialization
 * ============================================================ */
static int thread_pool_init(void)
{
    memset(&g_pool, 0, sizeof(g_pool));
    g_pool.shutdown_requested = 0;

    /* Initialize the work queue */
    g_pool.queue.head = 0;
    g_pool.queue.tail = 0;
    g_pool.queue.count = 0;
    g_pool.queue.total_submitted = 0;
    g_pool.queue.total_completed = 0;

    /* Create worker threads */
    for (long i = 0; i < THREAD_POOL_SIZE; i++) {
        int ret = pthread_create(&g_pool.threads[i], NULL,
                                 pool_worker, (void *)i);
        if (ret != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(ret));
            return -1;
        }
    }

    printf("[Pool] %d worker threads created\n", THREAD_POOL_SIZE);
    return 0;
}

/* ============================================================
 * Submit work to the pool
 * ============================================================ */
static int thread_pool_submit(void (*function)(void *), void *arg)
{
    if (g_pool.queue.count >= WORK_QUEUE_CAPACITY) {
        fprintf(stderr, "[Pool] Work queue full!\n");
        return -1;
    }

    work_item_t item = {
        .function = function,
        .arg      = arg,
        .job_id   = g_pool.queue.total_submitted
    };

    /* Enqueue */
    g_pool.queue.items[g_pool.queue.tail] = item;
    g_pool.queue.tail = (g_pool.queue.tail + 1) % WORK_QUEUE_CAPACITY;
    g_pool.queue.count++;
    g_pool.queue.total_submitted++;

    return 0;
}

/* ============================================================
 * Shutdown the pool — wait for workers to finish
 * ============================================================ */
static void thread_pool_shutdown(void)
{
    printf("[Pool] Shutdown requested. Waiting for workers...\n");
    g_pool.shutdown_requested = 1;

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(g_pool.threads[i], NULL);
    }

    printf("[Pool] All workers stopped. Submitted: %d, Completed: %d\n",
           g_pool.queue.total_submitted,
           g_pool.queue.total_completed);
}

/* ============================================================
 * Sample work functions — these are the "jobs" we submit
 * ============================================================ */
typedef struct {
    int    job_id;
    int    value;
    char   message[64];
} sample_job_arg_t;

static void sample_work(void *arg)
{
    sample_job_arg_t *job = (sample_job_arg_t *)arg;
    long tid = syscall(SYS_gettid);

    /* Simulate work with a short sleep */
    usleep(1000 + (rand() % 5000));  /* 1-6 ms */

    printf("    [Job %03d] Thread %ld processed: %s (value=%d)\n",
           job->job_id, tid, job->message, job->value);

    free(job);
}

/* ============================================================
 * Main — demonstrates thread pool usage
 * ============================================================ */
int main(void)
{
    srand(time(NULL));

    printf("=== Thread Pool Demo ===\n\n");

    /* Step 1: Initialize the pool */
    if (thread_pool_init() != 0) {
        return 1;
    }

    /* Give workers a moment to start */
    usleep(100000);  /* 100 ms */

    /* Step 2: Submit work items */
    int num_jobs = 20;
    printf("[Main] Submitting %d jobs...\n\n", num_jobs);

    for (int i = 0; i < num_jobs; i++) {
        sample_job_arg_t *job = malloc(sizeof(*job));
        if (!job) { perror("malloc"); break; }
        job->job_id = i;
        job->value  = i * i;
        snprintf(job->message, sizeof(job->message),
                 "compute_square(%d)", i);

        thread_pool_submit(sample_work, job);
    }

    /* Step 3: Wait for all work to complete
     * (In a real daemon, this would be an event loop.
     *  For this demo, we poll the completion counter.) */
    printf("\n[Main] Waiting for all jobs to complete...\n");
    while (g_pool.queue.total_completed < num_jobs) {
        usleep(10000);  /* 10 ms */
    }

    /* Step 4: Shutdown */
    printf("\n");
    thread_pool_shutdown();

    /* Step 5: Report */
    printf("\n=== Summary ===\n");
    printf("Submitted:  %d\n", g_pool.queue.total_submitted);
    printf("Completed:  %d\n", g_pool.queue.total_completed);
    if (g_pool.queue.total_submitted == g_pool.queue.total_completed) {
        printf("Result:     PASS (all jobs completed)\n");
    } else {
        printf("Result:     FAIL (lost %d jobs!)\n",
               g_pool.queue.total_submitted - g_pool.queue.total_completed);
        printf("NOTE: This is expected today! The race condition on\n");
        printf("      the shared queue will be fixed on Thursday with\n");
        printf("      mutex + condition variables.\n");
    }

    return 0;
}