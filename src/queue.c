//#include <unistd.h>
#include <stdlib.h>   // calloc()
#include <stdio.h>    // fprintf()
#include <assert.h>
#include <errno.h>
#include <string.h>   // strerror()

#include "queue.h"
#include "morre.h"

struct qi *qi_create(void){
    struct qi *item = calloc(1, sizeof(struct qi));
    if (!item){
        fprintf(stderr, "Memory allocation failure: failed to mallocate struct qi *\n");
        exit(MORRE_MEM_ALLOC);
    }

    return item;
}

void qi_destroy(struct qi *item, bool free_data){
    if (free_data) free(item->data);
    free(item);
}

struct qi *qi_dequeue(struct queue *queue){
    assert(queue);
    struct qi *res = NULL;
    
    if (!(queue->initialized & MTX_INITIALIZED)) return NULL; 
    assert(!pthread_mutex_lock(&queue->mtx));

    if (!queue->head){    // queue empty
        res = NULL;
        goto unlock;
    }

    res = queue->head;    // not empty
    queue->head = res->next;

    if(!queue->head){     // queue empty NOW
        queue->tail = NULL;
    }
    
unlock:
    assert(!pthread_mutex_unlock(&queue->mtx));
    return res;
}

/*
 * Create an queue item and add it to the queue. 
 * 
 * @threadsafe
 *
 * <-- return
 *     Always return NULL. This is to conform to the signature
 *     expected of a pthread-starting function, which returns `void *`.
 *     This function will be called as a thread-entry function by one of
 *     the mockit library functions.
 */
int qi_enqueue(struct queue *queue, void *arg, bool post_sem){
    assert(queue);
    struct qi *item = qi_create();
    item->data = arg;

    if (!(queue->initialized & MTX_INITIALIZED)){
        puts("uinitialized");
        return 2;
    }
    if (pthread_mutex_lock(&queue->mtx)){
        printf("pthread_lock() FAILED with %i, %s\n", errno, strerror(errno));
        return 2;
    }
    
    //printf("qneuqiring threadid \n");
    if (!queue->head){        // list empty : insert new node as both tail and head
        queue->head = item;
        queue->tail = item;
    }
    else{                     // queue not empty; make event the new queue tail
        queue->tail->next = item;
        queue->tail = item;
    }
    
    if (post_sem && (queue->initialized & SEM_INITIALIZED)){
        if (sem_post(&queue->sem)){
            fprintf(stderr, "%s\n", "failed to post event");
        }
    }
    
    assert(!pthread_mutex_unlock(&queue->mtx));
    
    return 0;
}

int queue_init(struct queue *queue, bool init_sem, bool init_mtx){
    assert(queue);

    if (init_sem){
        if (sem_init(&queue->sem, 0, 0)) return 1; 
        queue->initialized |= SEM_INITIALIZED;
    }

    if (init_mtx){
        if (pthread_mutex_init(&queue->mtx, NULL)){
            puts("returning 2");
            return 2;
        }
        queue->initialized |= MTX_INITIALIZED;
    }

    return 0;
}

int queue_destroy(struct queue *queue, bool destroy_sem, bool destroy_mtx){
    assert(queue);
    if (destroy_sem){
        if(sem_destroy(&queue->sem)) return 1;
        queue->initialized &= ~SEM_INITIALIZED;

    }

    if (destroy_mtx){
        if (pthread_mutex_destroy(&queue->mtx)) return 2;
        queue->initialized &= ~SEM_INITIALIZED;
    }

    return 0;
}

bool queue_isempty(struct queue *queue){
    assert(queue);
    return queue ? false : true;
}

