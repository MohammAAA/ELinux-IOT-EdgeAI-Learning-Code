/*
 * 10_race_demo.c — Demonstrate a race condition on a shared counter
 *
 * EXPECTED:  counter should be 4,000,000 (4 threads × 1,000,000 each)
 * ACTUAL:    counter will be LESS — different each run!
 *
 * Compile: gcc -Wall -Wextra -pthread -o bin/race_demo 10_race_demo.c
 * 
 * Detecting Race Conditions with ThreadSanitizer:
 *  GCC and Clang have a powerful tool called ThreadSanitizer (TSan) that detects data races at runtime:
 *  # Compile with -fsanitize=thread
 *  gcc -Wall -Wextra -pthread -fsanitize=thread -g -o bin/race_demo 10_race_demo.c
 *  # Run it — TSan will report EVERY race it finds
 *  ./bin/race_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#define NUM_THREADS    4
#define INCREMENTS     1000000

/* Shared global variable — ALL threads see this */
static int g_counter = 0;

/* ============================================================
 * Thread function: increment g_counter INCREMENTS times
 * ============================================================ */
static void *incrementer(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < INCREMENTS; i++) {
        g_counter++;    /* ← THIS IS THE RACE CONDITION */
    }
    printf("Thread %ld: finished %d increments\n", id, INCREMENTS);
    return NULL;
}

int main(void)
{
    pthread_t threads[NUM_THREADS];

    printf("Starting %d threads, each incrementing %d times...\n",
           NUM_THREADS, INCREMENTS);
    printf("Expected final counter: %d\n\n", NUM_THREADS * INCREMENTS);

    /* Create all threads */
    for (long i = 0; i < NUM_THREADS; i++) {
        int ret = pthread_create(&threads[i], NULL, incrementer, (void *)i);
        if (ret != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(ret));
            return 1;
        }
    }

    /* Wait for all threads to finish */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\nActual final counter:   %d\n", g_counter);
    printf("Missing:                %d\n",
           (NUM_THREADS * INCREMENTS) - g_counter);

    if (g_counter == NUM_THREADS * INCREMENTS) {
        printf("PASS: No race condition detected (lucky!)\n");
    } else {
        printf("FAIL: Race condition! Lost %d increments!\n",
               (NUM_THREADS * INCREMENTS) - g_counter);
    }

    return 0;
}