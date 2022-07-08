#ifndef MOCKIT_Q_H__
#define MOCKIT_Q_H__

#include <stdbool.h>
#include <semaphore.h>
#include <pthread.h>

/* used by queue functions to test if the embedded mutex or
 * semaphore are initialized before using them. Initializing
 * them is optional on a per-queue basis.
 */
#define SEM_INITIALIZED 0x1
#define MTX_INITIALIZED 0x2

/* 
 * an event in this context is an entry in an event queue, associated 
 * with (and added as a result of the expiry of a) timer (either 
 * one-shot or an interval timer).
 *
 * Each event points to a `struct data`, which in turn, among others, 
 * contains a callback. Dequeing and 'handling' the event means removing
 * it from the event queue and calling its callback function.
 */
struct qi{
    void *data;
    struct qi *next;
};

/*
 * 'events' (dynamically allocated `struct event` types) get put here. 
 * An event is added when created, and removed when handled.
 */
struct queue{
    struct qi *head, *tail;
    short initialized;  // SEM_INITIALIZED | MTX_INITIALIZED
    sem_t sem;
    pthread_mutex_t mtx;
};

/*
 * Initialize queue (must be statically, not dynamically, allocated)
 * by zeroing it out.
 *
 * --> init_sem
 *     if true, initialize queue->sem 
 *
 * --> init_mtx
 *     if true, initialize queue->mtx
 *
 * <-- return
 *     0 on success; 1 for failing to initialize queue->sem,
 *     2 for failing to initialize queue->mtx
 */
int queue_init(struct queue *queue, bool init_sem, bool init_mtx);

/*
 * The reverse of queue_init(). The return values are analogous
 * to those there.
 */
int queue_destroy(struct queue *queue, bool destroy_sem, bool destroy_mtx);

/* 
 * Create and initialize a queue item for insertion into queue.
 *
 * <-- return
 *     Dynamically-allocated and initialized `struct qi` object.
 *     The object is initialized by having its memory zeroed out
 *     courtesy of calloc().
 */
struct qi *qi_create(void);

/*
 * Destroy (free) a queue item.
 * 
 * --> item
 *     A dynamically-allocated `struct qi *`.
 *
 *  NOTES
 * -------
 * item->data is free()'d separately by lua_process_events()
 * right before this function is called. 
 */
void qi_destroy(struct qi *item, bool free_data);

/*
 * Dequeue queue item from the head end of the queue.
 * 
 * Remove a queue item from the queue QUEUE
 * and return it. It's then the responsibility of
 * the caller to call event_destroy() on the value
 * returned by this function when no longer needed.
 *
 * --> queue
 *     the event queue to dequeue an item from (or NULL when empty).
 * 
 * <-- return 
 *     a dynamically allocated `struct qi *` (NULL if the queue
 *     is empty).
 */
struct qi *qi_dequeue(struct queue *queue);

/*
 * Create an event item and add it to the queue. 
 * 
 * @threadsafe
 *
 * <-- return
 *     Always return NULL. This is to conform to the signature
 *     expected of a pthread-starting function, which returns `void *`.
 *     This function will be called as a thread-entry function by one of
 *     the mockit library functions.
 */
int qi_enqueue(struct queue *queue, void *arg, bool post_sem);



bool queue_isempty(struct queue *queue);

#endif
