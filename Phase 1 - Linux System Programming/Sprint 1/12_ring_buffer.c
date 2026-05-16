/*
 * 12_ring_buffer.c — Thread-safe circular buffer for producer/consumer pattern
 *
 * Features:
 *   - Fixed-capacity (fixed size), pre-allocated buffer
 *   - Thread-safe: protected by mutex + condition variables
 *   - Supports multiple producers and multiple consumers
 *   - Blocking enqueue (waits when full) and dequeue (waits when empty)
 *   - Clean shutdown support
 *
 * Test with: ring_buffer_test.c (2 producers, 3 consumers)
 *
 * Compile: gcc -Wall -Wextra -pthread -c 12_ring_buffer.c
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "12_ring_buffer.h"

/* ============================================================
 * Initialization
 * ============================================================ */
int ring_buffer_init(ring_buffer_t *rb)
{
    memset(rb, 0, sizeof(ring_buffer_t));

    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->total_enqueued = 0;
    rb->total_dequeued = 0;
    rb->shutdown = 0;

    int ret;

    ret = pthread_mutex_init(&rb->mutex, NULL);
    if (ret != 0) return ret;

    ret = pthread_cond_init(&rb->cond_not_empty, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&rb->mutex);
        return ret;
    }

    ret = pthread_cond_init(&rb->cond_not_full, NULL);
    if (ret != 0) {
        /* Initialization failed, destroy all other initialized thread safety requirements */
        pthread_cond_destroy(&rb->cond_not_empty);
        pthread_mutex_destroy(&rb->mutex);
        return ret;
    }

    return 0;
}

/* ============================================================
 * Cleanup
 * ============================================================ */
void ring_buffer_destroy(ring_buffer_t *rb)
{
    pthread_cond_destroy(&rb->cond_not_full);
    pthread_cond_destroy(&rb->cond_not_empty);
    pthread_mutex_destroy(&rb->mutex);
}

/* ============================================================
 * Enqueue — called by PRODUCER threads
 *
 * If the buffer is full, this function BLOCKS until space is available.
 *
 * Returns 0 on success, -1 on shutdown.
 * ============================================================ */
int ring_buffer_enqueue(ring_buffer_t *rb, const ring_item_t *item)
{
    pthread_mutex_lock(&rb->mutex);

    /* ──────────────────────────────────────────────────────────
     * THE CRITICAL while() LOOP — never if()!
     *
     * Three reasons:
     *   1. Spurious wakeups "pthread_cond_wait() may return EVEN IF no one signaled." (POSIX allows them)
     *   2. Multiple producers might be woken for one slot (one signal wakes all waiting)
     *   3. Another producer might fill the slot between signal
     *      and this thread actually waking up
     * ────────────────────────────────────────────────────────── */
    while (rb->count == RB_CAPACITY && !rb->shutdown) {
        pthread_cond_wait(&rb->cond_not_full, &rb->mutex);
    }

    if (rb->shutdown) {
        pthread_mutex_unlock(&rb->mutex);
        return -1;  /* Shutting down, don't enqueue */
    }

    /* Place item at tail position */
    rb->buffer[rb->tail] = *item;
    rb->tail = (rb->tail + 1) % RB_CAPACITY;  /* Wrap around as it is circular buffer */
    rb->count++;
    rb->total_enqueued++;

    /* Signal ONE consumer that data is available.
     * Use signal (not broadcast) because we added ONE item,
     * so only ONE consumer needs to wake up. */
    pthread_cond_signal(&rb->cond_not_empty);

    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

/* ============================================================
 * Dequeue — called by CONSUMER threads
 *
 * If the buffer is empty, this function BLOCKS until data arrives.
 *
 * Returns 0 on success, -1 on shutdown (buffer empty and
 * shutdown was requested).
 * ============================================================ */
int ring_buffer_dequeue(ring_buffer_t *rb, ring_item_t *item)
{
    pthread_mutex_lock(&rb->mutex);

    /* ──────────────────────────────────────────────────────────
     * THE CRITICAL while() LOOP — never if()!
     *
     * Wait until buffer is NOT empty OR shutdown is requested.
     * ────────────────────────────────────────────────────────── */
    while (rb->count == 0 && !rb->shutdown) {
        pthread_cond_wait(&rb->cond_not_empty, &rb->mutex);
    }

    if (rb->count == 0 && rb->shutdown) {
        /* Buffer is empty AND we're shutting down, no more data */
        pthread_mutex_unlock(&rb->mutex);
        return -1;
    }

    /* If we get here, then the count > 0 --> there IS data */
    *item = rb->buffer[rb->head];
    rb->head = (rb->head + 1) % RB_CAPACITY;  /* Wrap around */
    rb->count--;
    rb->total_dequeued++;

    /* Signal ONE producer that space is available */
    pthread_cond_signal(&rb->cond_not_full);

    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

/* ============================================================
 * Shutdown — signal all waiting threads to exit
 * ============================================================ */
void ring_buffer_shutdown(ring_buffer_t *rb)
{
    pthread_mutex_lock(&rb->mutex);
    rb->shutdown = 1;

    /* Wake ALL waiting threads — both producers and consumers.
     * They'll check the shutdown flag and exit. */
    pthread_cond_broadcast(&rb->cond_not_empty);
    pthread_cond_broadcast(&rb->cond_not_full);

    pthread_mutex_unlock(&rb->mutex);
}

/* ============================================================
 * Diagnostic: print buffer state (call with mutex held or
 * in a single-threaded context)
 * ============================================================ */
void ring_buffer_print_state(const ring_buffer_t *rb)
{
    printf("  Buffer: head=%d, tail=%d, count=%d/%d, "
           "enqueued=%d, dequeued=%d, shutdown=%d\n",
           rb->head, rb->tail, rb->count, RB_CAPACITY,
           rb->total_enqueued, rb->total_dequeued,
           rb->shutdown);
}