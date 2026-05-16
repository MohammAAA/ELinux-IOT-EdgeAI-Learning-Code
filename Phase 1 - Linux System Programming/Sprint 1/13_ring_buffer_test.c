/*
 * 13_ring_buffer_test.c — Test the ring buffer with 2 producers + 3 consumers
 *
 * Gate criterion: "13_ring_buffer_test.c consumes exactly N items with 0 lost"
 *
 * Compile: gcc -Wall -Wextra -pthread -o ./bin/ring_buffer_test \
 *            12_ring_buffer.c 13_ring_buffer_test.c
 *
 * Run:     ./bin/ring_buffer_test
 */

#define _GNU_SOURCE /* If we use "#define _POSIX_C_SOURCE 200809L", then the usleep() and syscall(SYS_***) will be prohibited as they are not in POSIX standard, and the macro _POSIX_C_SOURCE strictly enforces the POSIX standard*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "12_ring_buffer.h"   /* Or compile together */

#define NUM_PRODUCERS   2
#define NUM_CONSUMERS   3
#define ITEMS_PER_PRODUCER 50000

/* Shared ring buffer */
static ring_buffer_t g_rb;

/* Per-consumer tracking */
typedef struct {
    int          consumer_id;
    int          items_consumed;
    long long    min_sequence;
    long long    max_sequence;
} consumer_stats_t;

/* ============================================================
 * Producer Thread
 * ============================================================ */
typedef struct {
    int producer_id;
    int num_items;
} producer_args_t;

static void *producer(void *arg)
{
    producer_args_t *args = (producer_args_t *)arg;
    //pthread_t tid = pthread_self();
    long tid = syscall(SYS_gettid);

    printf("[Producer %d] Starting (tid=%ld, items=%d)\n",
           args->producer_id, tid, args->num_items);

    for (int i = 0; i < args->num_items; i++) {
        ring_item_t item;
        item.sequence    = args->producer_id * 1000000 + i;
        item.producer_id = args->producer_id;
        snprintf(item.data, sizeof(item.data),
                 "P%d-%06d", args->producer_id, i);

        if (ring_buffer_enqueue(&g_rb, &item) == -1) {
            printf("[Producer %d] Shutting down (enqueued %d/%d)\n",
                   args->producer_id, i, args->num_items);
            break;
        }
    }

    printf("[Producer %d] Finished (enqueued %d items)\n",
           args->producer_id, args->num_items);

    free(args);
    return NULL;
}

/* ============================================================
 * Consumer Thread
 * ============================================================ */
typedef struct {
    int               consumer_id;
    consumer_stats_t *stats;
} consumer_args_t;

static void *consumer(void *arg)
{
    consumer_args_t *args = (consumer_args_t *)arg;
    //pthread_t tid = pthread_self(); // this creates non-human friendly tid
    long tid = syscall(SYS_gettid);
    consumer_stats_t *stats = args->stats;

    stats->consumer_id    = args->consumer_id;
    stats->items_consumed = 0;
    stats->min_sequence   = __LONG_LONG_MAX__;
    stats->max_sequence   = 0;

    printf("[Consumer %d] Starting (tid=%ld)\n",
           args->consumer_id, tid);

    while (1) {
        ring_item_t item;

        if (ring_buffer_dequeue(&g_rb, &item) == -1) {
            /* Shutdown or empty + shutdown */
            break;
        }

        stats->items_consumed++;

        if ((long long)item.sequence < stats->min_sequence)
            stats->min_sequence = item.sequence;
        if ((long long)item.sequence > stats->max_sequence)
            stats->max_sequence = item.sequence;

        /* Simulate a tiny bit of work (processing the item) */
        if (stats->items_consumed % 10000 == 0) {
            /* Print progress every 10,000 items */
        }
    }

    printf("[Consumer %d] Finished (consumed %d items)\n",
           args->consumer_id, stats->items_consumed);

    free(args);
    return NULL;
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void)
{
    int total_items = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

    printf("=== Ring Buffer Test ===\n");
    printf("Producers: %d, Consumers: %d\n", NUM_PRODUCERS, NUM_CONSUMERS);
    printf("Items per producer: %d, Total: %d\n", ITEMS_PER_PRODUCER, total_items);
    printf("Buffer capacity: %d\n\n", RB_CAPACITY);

    /* Initialize ring buffer */
    if (ring_buffer_init(&g_rb) != 0) {
        fprintf(stderr, "ring_buffer_init failed\n");
        return 1;
    }

    /* Create consumer threads */
    pthread_t consumer_threads[NUM_CONSUMERS];
    consumer_stats_t consumer_stats[NUM_CONSUMERS];

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        consumer_args_t *args = malloc(sizeof(*args));
        args->consumer_id = i;
        args->stats       = &consumer_stats[i];

        int ret = pthread_create(&consumer_threads[i], NULL,
                                 consumer, args);
        if (ret != 0) {
            fprintf(stderr, "pthread_create consumer: %s\n", strerror(ret));
            return 1;
        }
    }

    /* Small delay to let consumers start and block on empty buffer */
    usleep(50000);

    /* Create producer threads */
    pthread_t producer_threads[NUM_PRODUCERS];

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_args_t *args = malloc(sizeof(*args));
        args->producer_id = i;
        args->num_items    = ITEMS_PER_PRODUCER;

        int ret = pthread_create(&producer_threads[i], NULL,
                                 producer, args);
        if (ret != 0) {
            fprintf(stderr, "pthread_create producer: %s\n", strerror(ret));
            return 1;
        }
    }

    /* Wait for all producers to finish */
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producer_threads[i], NULL);
    }

    printf("\n[Main] All producers finished. Shutting down buffer...\n");

    /* Shutdown the ring buffer — wakes all consumers */
    ring_buffer_shutdown(&g_rb);

    /* Wait for all consumers to finish */
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        pthread_join(consumer_threads[i], NULL);
    }

    /* === RESULTS === */
    printf("\n");
    printf("═══════════════════════════════════════════════\n");
    printf("                 RESULTS                       \n");
    printf("═══════════════════════════════════════════════\n");

    int total_consumed = 0;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        total_consumed += consumer_stats[i].items_consumed;
        printf("Consumer %d: consumed %6d items\n",
               consumer_stats[i].consumer_id,
               consumer_stats[i].items_consumed);
    }

    printf("───────────────────────────────────────────────\n");
    printf("Total enqueued:    %d\n", g_rb.total_enqueued);
    printf("Total dequeued:    %d\n", g_rb.total_dequeued);
    printf("Total consumed:    %d\n", total_consumed);
    printf("Items LOST:        %d\n", g_rb.total_enqueued - total_consumed);
    printf("───────────────────────────────────────────────\n");

    int expected = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

    if (total_consumed == expected && g_rb.total_enqueued == total_consumed) {
        printf("RESULT:  *** PASS *** — 0 items lost!\n");
    } else {
        printf("RESULT:  *** FAIL *** — %d items lost!\n",
               expected - total_consumed);
    }

    printf("═══════════════════════════════════════════════\n\n");

    /* Cleanup */
    ring_buffer_destroy(&g_rb);

    return (total_consumed == expected) ? 0 : 1;
}