/* ============================================================
 * Configuration
 * ============================================================ */
#define RB_CAPACITY 1024    /* Ring buffer size, must be a power of 2 for efficiency */

/* ============================================================
 * Ring Buffer Item — what we store in the buffer
 * ============================================================ */
typedef struct {
    int   sequence;      /* Sequence number for tracking */
    int   producer_id;   /* Which producer created this */
    char  data[56];      /* Payload, 56 bytes */
} ring_item_t;

/* ============================================================
 * Ring Buffer Structure
 * ============================================================ */
typedef struct {
    ring_item_t     buffer[RB_CAPACITY];  /* The circular array */

    int             head;                  /* Next item to dequeue */
    int             tail;                  /* Next slot to enqueue */
    int             count;                 /* Current number of items */

    int             total_enqueued;        /* Lifetime counter */
    int             total_dequeued;        /* Lifetime counter */

    int             shutdown;              /* Flag for clean shutdown */

    /* Synchronization primitives */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_not_empty;  /* Consumers wait here when empty */
    pthread_cond_t  cond_not_full;   /* Producers wait here when full */
} ring_buffer_t;


/* ============================================================
 * Initialization
 * ============================================================ */
int ring_buffer_init(ring_buffer_t *rb);

/* ============================================================
 * Cleanup
 * ============================================================ */
void ring_buffer_destroy(ring_buffer_t *rb);

/* ============================================================
 * Enqueue
 * ============================================================ */
int ring_buffer_enqueue(ring_buffer_t *rb, const ring_item_t *item);

/* ============================================================
 * Dequeue
 * ============================================================ */
int ring_buffer_dequeue(ring_buffer_t *rb, ring_item_t *item);

/* ============================================================
 * Shutdown
 * ============================================================ */
void ring_buffer_shutdown(ring_buffer_t *rb);