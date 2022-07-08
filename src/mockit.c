#include <errno.h>
#include <string.h>      // memset()
#include <time.h>        // clock_gettime(), clock_nanosleep() etc
#include <stdint.h>      // fixed width int types e.g. uint32_t
#include <stdbool.h>
#include <assert.h> 
#include <stdlib.h>      // free()
#include <signal.h>      // union sigval
#include <stdio.h>

#include "mockit.h"
#include "morre.h"
#include "queue.h"

int Mockit_timerq_delete(pthread_t thread_id);

/*
 * Weirdly, the `struct timespec` passed to clock_nanosleep() stores
 * the nanoseconds (tv_nsec) in a long. However, clock_nanosleep() says it will 
 * return EINVAL if the the tv_nsec value it gets passed is > 999999999, which is 
 * in actuality MUCH less than MAX_LONG.
 */
#define MAX_NS_VAL 999999999

/* Unused in this compilation unit */
UNUSED(static pthread_mutex_t cbmtx) = PTHREAD_MUTEX_INITIALIZER;     // callback mutex

//===================================
// ---- Type definitions -------
//===================================

/* A callback timer suitable for adding to the timer object queue */
typedef struct qi cbtimer_t;

/* callback function */
typedef void (* timedcb)(void *);

//===================================
// ---- File-scoped Variables -------
//===================================
struct queue timerq;


//===================================
// ---- Function definitions  -------
//===================================

/* 
 * Given a value in milliseconds, correctly populate the `struct timespec`
 * provided with seconds and nanoseconds.
 *
 * The chalenges and difficulties of correctly populating the timespec
 * structure is that while `.tv_nsec` is a long, its maximum acceptable
 * value is in actuality much smaller than MAX_LONG. If one overshoots
 * this accepted value, as described in e.g. the man page of `clock_nanosleep()`
 * for example, EINVAL is normally returned. 
 * 
 * This function ensures that given a value in milliseconds, it is correctly 
 * added to the seconds and nanoseconds values in the `struct timespec` 
 * structure. 
 *
 * FMI see MAX_NS_VAL above and the comments below in the body of the function.
 */
void timespec_add_ms__(struct timespec *ts, uint32_t milliseconds){
    assert(ts);

    // convert ms to secs and ns
    uint32_t  secs = milliseconds/ MS_IN_SECS; // 0 <= secs
    uint32_t  msecs = milliseconds % MS_IN_SECS;
    uint32_t  nsecs = msecs * NS_IN_MS; // time is given in ms but nanosleep expect ns

    (*ts).tv_sec += secs;
   
    /* the number of nanoseconds the way it's implemented in Mockit, will
       always be < 1 second. That is, it can/should be at most 999999999,
       (see MAX_NS_VAL defined above for this purpose) which is 1 nanosecond 
       from a full second.
    */
    assert(nsecs < MAX_NS_VAL);

    /* If you go beyond MAX_NS_VAL, e.g. clock_nanosleep() WILL return an error.
       Therefore care must be taken we always stay within the range. If our value
       of nanoseconds > MAX_NS_VAL, we should convert it to seconds and put it in
       tv_sec, and only leave the remainder in tv_nsec.
    */
    long ns_free_space = MAX_NS_VAL - (*ts).tv_nsec;
    // there's enough space to accomodate nsecs
    if (ns_free_space >= nsecs){
        (*ts).tv_nsec += nsecs;    
    }
    // not enough space: chop off the full second(s)
    else{
        nsecs += (*ts).tv_nsec;
        uint32_t num_secs = nsecs / NS_IN_SECS; 
        (*ts).tv_sec += num_secs;
        
        nsecs = nsecs % NS_IN_SECS;
        assert(nsecs <= MAX_NS_VAL);
        (*ts).tv_nsec = nsecs;
    }
}

/*
 * Make a blocking call to clock_nanosleep to sleep for TIME milliseconds. 
 *
 *
 * In order for the call to be blocking, the sleep takes place in the same
 * thread as the caller, not a different one. 
 *
 * --> time
 *     Duration to sleep for in milliseconds.
 *
 * --> do_restart
 *     If true, resume sleeping if interrupted by a signal. If false,
 *     save the remaining time (if any) in 'time_left' on any interruption
 *     and return.
 * 
 * --> time_left, [optional]
 *     Time left (in milliseconds) until sleep woud've been completed. 
 *     0 if the sleep has been successfully completed. If do_restart
 *     is true, the sleep always completes and therefore time_left
 *     is always 0 in that case and so the parameter is ignored by 
 *     this function and should likewise be ignored by the caller and
 *     can be safely specified as NULL.
 *     More generally, time_left is an optional parameter and the caller
 *     can always specify it as NULL if they are uninterested in the info.
 *     
 * <-- return
 *     error code returned by clock_nanosleep() (0 on success).
 *
 * NOTES
 * --------
 * For better precision, one can just call clock_nanosleep() directly.
 * 
 * IFF do_restart=True, the sleep is resumed if interrupted by a signal. 
 * In other words, this function will only return once the sleep has 
 * been completed (assuming no errors preventing that).
 *
 * clock_nanosleep() rather than sleep() is used so that it can be mixed
 * together with signals (the older sleep() is not suitable for that) as 
 * well as to allow for much better (nanosecond) precision.
 *
 * clock_nanosleep(), unlike nanosleep(), is Linux-specific, not POSIX-mandated,
 * so this function is not portable across unices. However, it is used
 * instead of nanosleep() because it avoids time drift (possible when 
 * repeatedly restarting sleep after signal interrupts) by allowing the 
 * specification of an absolute time value rather than a relative duration.
 */
int Mockit_bsleep(uint32_t milliseconds, bool do_restart, uint32_t *time_left){
    int res = 0;
    struct timespec to_sleep; // current time + milliseconds
    memset(&to_sleep, 0, sizeof(struct timespec));

    if (clock_gettime(CLOCK_REALTIME, &to_sleep) == -1){
        if (time_left) *time_left = milliseconds;  // have not slept at all
        return -1;
    }

    // populate the struct timespect correctly, ensuring we stay within the
    // correct range for tv_nsec
    timespec_add_ms__(&to_sleep, milliseconds);

    // 1)
    // do resume sleep when interrupted by signals => time_left unused
    if (do_restart){
        // rem is not used when TIMER_ABSTIME is used
        res = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &to_sleep, NULL);

        if (res){
            if (res == EINTR){
                while (res == EINTR){
                    res = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &to_sleep, NULL);
                }
            }

            else{
                printf("res received: %i\n", res);
                //printf("res received: %i\n", EINVAL);
                if (time_left) *time_left = milliseconds;
            }
        }
    }

    // 2)
    // do NOT restart sleep when interrupted by signals => time_left used
    // use relative duration not absolute time value
    else{
        struct timespec rem;
        memset(&rem, 0, sizeof(rem));

        res = clock_nanosleep(CLOCK_REALTIME, 0, &to_sleep, &rem);

        if (res == EINTR){
            int32_t ms = 0;
            ms += rem.tv_sec * MS_IN_SECS;
            ms += rem.tv_nsec / NS_IN_MS;
            if (time_left) *time_left = ms;
        }
    }
    return res;  // 0 on success, otherwise clock_nanosleep() errror code
}

/* 
 * Sleep for ctx->timeout milliseconds before calling ctx->cb().
 * This function is used for one-shot and interval timers.
 * 
 * This function is called to sleep in a different thread separate
 * from the main one so that the library isn't blocked by the sleep.
 * If interrupted by a signal, it will resume sleeping until done.
 *
 * ctx->cb MUST be threadsafe or reentrant; it's the responsibility
 * of the library user to ensure that's the case.
 *
 * <-- return
 *     NULL is always returned. The function simply has this signature
 *     because it's required by PTHREADS for thread-starting functions.
 */

static void *oneshot__(void *ctx){
    /* 
     * one-off timers sleep ONCE for dt->timeout time
     * then call dt->cb and then self-destruct.
     */
    struct data *dt = ctx;  
    assert(Mockit_bsleep(dt->timeout, true, NULL) == 0);
    dt->cb(dt);
    Mockit_destroy(dt, dt->free_data ? true : false, dt->free_ctx ? true : false);

    return NULL;
}

/*
 * interval timers first 1) add themselves to timerq, then 2) sleep for for dt->timeout, 
 * then 3) call dt->cb, then 4) IFF the timer has been mfd, they self-destruct, otherwise
 * they go back to 2).
 */
static void *cycle__(void *ctx){
    struct data *dt = ctx;  
    printf("timer with id %lu\n", dt->thread_id__);

    if (!dt->enqueued && !dt->mfd){
        int res = qi_enqueue(&timerq, dt, false);
        dt->enqueued = true;
        printf("res is %i\n", res);
        if (res) exit(EXIT_FAILURE);
    }

    int mfd = 0;
    uint32_t interval = dt->timeout;
    timedcb callback = dt->cb;
   
while (!mfd){
    assert(Mockit_bsleep(interval, true, NULL) == 0);

    assert(!pthread_mutex_lock(&timerq.mtx));
    mfd = dt->mfd;
    assert(!pthread_mutex_unlock(&timerq.mtx));

    if (mfd) break;
    callback(dt);

    assert(!pthread_mutex_lock(&timerq.mtx));
    assert(dt);
    mfd = dt->mfd;
    assert(!pthread_mutex_unlock(&timerq.mtx));
}
    puts("got marked for death: will delete and destroy");
    // otherwise, the timer has been marked for death,
    // so free its resources, and dequeue it
    Mockit_timerq_delete(dt->thread_id__);

    Mockit_destroy(dt, dt->free_data ? true : false, dt->free_ctx ? true : false);

    return NULL;
}

/*
 * Call a callback ONCE on timer expiration.
 * 
 * --> time
 *     time, in milliseconds, after which to call data->cb. 
 *
 * --> data
 *     a pointer to a caller-allocated `struct data`;
 *     As well as allocating it, this is meant to also be partially 
 *     populated by the caller;
 *
 * --> data->cb
 *     a callback function that returns a void pointer and takes
 *     a void pointer; mockit WILL always call the callback with a 
 *     `struct data` pointer so it must be cast and interpreted as appropriate.
 *     The caller must ensure the callback is reentrant or otherwise 
 *     thread-safe.
 *
 * <-- return
 *     1 on failure, 0 on success;
 */
int Mockit_oneoff(uint32_t time, struct data *data){
    if (!data) return -1;   // data should've been allocated and partly populated by caller

    // create a thread that will start life by calling sleeper__; this
    // will sleep for TIME duration before calling data->cb() with
    // 'data' as an argument; the thread will store its id in thread_id and will 
    // use default thread attributes.
    data->timeout = time;
    data->is_cyclic__ = false;   // one-off, not cyclical
    data->mfd  = false;  // not marked for death

    if(pthread_create(&data->thread_id__, NULL, oneshot__, data)) return 1;
    if (pthread_detach(data->thread_id__)) return 1;
    
    return 0;
}

/*
 * Arrange for data->cb() to be called on expiration of POSIX interval timer. 
 *
 * --> interval 
 *     the value of the interval, in milliseconds, at which the timer 
 *     is to expire and the callback in data->cb is to be called.
 *
 * <-> data
 *     a `struct data *` (see definition in mockit.h).
 *     This must be dynamically allocated by the caller and the 
 *     caller must also partially populate it with various pieces of
 *     data, including an appropriate callback function (assigned
 *     to data->cb) to be called on interval expiry.
 *
 *     DATA is further manipulated and populated by this function before
 *     being returned to the caller - on success.
 *     The timer id of the interval timer created is written to data->timer_id.
 *     The caller can destroy the associated interval timer by calling Mockit_destroy()
 *     on this id (see Mockit_destroy()).
 *     On failure, data is set to NULL.
 *
 * <-- return
 *     0 on success, and the value of errno on error/failure.
 *    
 *  NOTES
 * -------
 * If interrupted by a signal, POSIX timers are _not_ stopped (unless of course
 * the process exits): this is not because of anything Mockit_getit() does - that's 
 * just how POSIX interval timers behave.
 */
int Mockit_getit(uint32_t interval, struct data *data){
    if (!data) return -1;   // data should've been allocated and partly populated by caller

    // create a thread that will start life by calling sleeper__; this
    // will sleep for TIME duration before calling data->cb() with
    // 'data' as an argument; the thread will store its id in thread_id and will 
    // use default thread attributes.
    data->timeout = interval;
    data->is_cyclic__ = true; 
    data->mfd  = false;  // not marked for death

    if(pthread_create(&data->thread_id__, NULL, cycle__, data)) return 1;
    if (pthread_detach(data->thread_id__)) return 1;
    
    return 0;
}

/*
 * Free the dynamic user-allocated memory for the `struct data *`.
 * 
 * If `->is_cyclic__` is true, also call timer_delete on the timer_id field 
 * to disarm and destroy the assumed associated interval timer. Otherwise if
 * `->is_cyclic__` is false, assume one-shot timer and forego the call to 
 * timer_delete() (which would otherwise segfault!).
 *
 * --> dt
 *     a `struct data *` dynamically allocated by the user (caller) previously.
 *     timer_delete() gets called on `dt->timer_id` to first destroy the interval 
 *     timer object (which of course disarms it as well), before calling free() on 
 *     `dt` itself.
 *     NOTE: timer_delete() only gets called on `dt->timer_id` IFF `dt->is_cyclic__`
 *     is true i.e. if the timer is an interval timer, not a one-off one.
 *
 * --> free_data
 *     If true, then call free() on the (dynamically allocated) dt, else assume dt 
 *     has static storage and simply zero it out.
 *
 * <-- return
 *     errno value set by timer_delete().
 *
 * Subtleties
 * ------------------------------
 * after timer_delete() returns, there is a warranty that no new 
 * notifications will happen, but there could be any number of 
 * notification still being processed in their own thread.
 * This means there's a distinct risk that if you free dt
 * in this function, the pending threads that will STILL
 * call the callback could execute dt->cb() on the freed
 * dt, therefore performing an illegal read (use after free)
 * by using a dangling pointer.
 */
int Mockit_destroy(struct data *dt, bool free_data, bool free_ctx){
    assert(dt);
    
    printf("called with free_data=%i, free_ctx= %i\n", free_data, free_ctx);
    // for one-off timers the dt->timer_id will be invalid, since it was never set.
    if (free_ctx) free(dt->ctx);

    if (free_data){
        free(dt);
    }
    //else{
    //    memset(dt, 0, sizeof(struct data));
    //}
    return 0;
}

/* 
 * Dynamically allocate a `struct data *` and zero it out.
 *
 * The client should call this to get a data struct to pass to 
 * Mockit_oneoff() and Mockit_getit().
 *
 * --> cb
 *     a callback with the specified signature that is to be called on
 *     on exiration of one-shot or interval timers.
 *
 * --> timeout
 *     The duration (either for a one-shot timer or for an interval timer)
 *     to wait before calling the callback.
 *
 * --> ctx
 *     The callback function will be passed this value as argument when called. 
 *
 * <-- return
 *     Dynamically allocated `struct data *`. The blocked has been zeroed
 *     out and is ready to be used by the caller by being passed around
 *     to the other functions here expecting it mentioned above.
 */
struct data *Mockit_dynamic_data_init(void (*cb)(void *), 
                                      uint32_t timeout, 
                                      void *ctx,
                                      bool free_data,
                                      bool free_ctx)
{
    struct data *dt = calloc(1,sizeof(struct data));
    assert(dt);
    assert(cb);

    dt->cb = cb;
    dt->timeout = timeout;
    dt->ctx = ctx;
    dt->free_ctx = free_ctx;
    dt->free_data = free_data;

    return dt;
}

/*
 * Initialize a statically declared struct data.
 *
 * See Mockit_dynamic_data_init fmi.
 */
void Mockit_static_data_init(struct data *dt, 
                                     void (*cb)(void *),
                                     uint32_t timeout, 
                                     void *ctx,
                                     bool free_data,
                                     bool free_ctx)
{
    assert(dt);
    assert(cb);

    memset(dt, 0, sizeof(struct data));

    dt->cb = cb;
    dt->timeout = timeout;
    dt->ctx = ctx;
    dt->free_ctx = free_ctx;
    dt->free_data = free_data;
}

/*
 * Return a time tuple of (seconds, milliseconds).
 *
 * <-- secs, @out
 *     Number of seconds as since the epoch (i.e. a Unix timestamp).
 *
 * <-- ms, @out
 *     millieseconds since the last second.
 *
 * <-- return
 *     0 on success, the value of errno on error.
 *
 * Note that either or both of secs and ms can be NULL.
 * If both are NULL, the call is essentially useless as no time
 * parameter is returned. If secs is NULL but ms is not NULL, 
 * only the number of milliseconds since the last second is returned.
 * If secs is not NULL but ms is NULL, a normal unix timestamp is returned,
 * as returned by time().
 */
int Mockit_gettime(time_t *secs, long *ms){
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)){
        return errno;
    }
    
    if (secs) *secs = ts.tv_sec;
    if (ms) *ms = ts.tv_nsec / NS_IN_MS;
    
    return 0;
}

/*
 * Get a millisecond timestamp since the epoch.
 *
 * The standard unix timestamp as returned by time()
 * represents the number of seconds since the epoch.
 * However it is sometimes the case that millisecond precision
 * is needed instead. That's what this function does.
 *
 * <-- timestamp
 *     The number of milliseconds since the epoch.
 *
 * <-- return
 *     1 on error, 0 on success.
 */
int Mockit_mstimestamp(uint64_t *timestamp){
    time_t secs = 0;
    time_t msecs = 0; // will be less than 1000

    if(Mockit_gettime(&secs, &msecs)) return 1;

    *timestamp = secs * 1000 + msecs;

    return 0;
}

int Mockit_init(void){
    int res = queue_init(&timerq, false, true);
    printf("queue->initalized=%i\n", timerq.initialized);
    return res;
}

/*
 * Mark for death the timer with thread_id in the timerq.
 */
int Mockit_disarm(pthread_t thread_id){
    int res = 1;
    if (!(timerq.initialized & MTX_INITIALIZED)) return 1;
    assert(!pthread_mutex_lock(&timerq.mtx));
    
    cbtimer_t *current = NULL;
    printf("looking to disarm thread if %lu\n", thread_id);
    for (current = timerq.head; current; current = current->next){
        struct data *timer = current->data;
        if (timer->thread_id__ == thread_id){
            puts(".. and found it");
            timer->mfd = true;
            res = 0;
        }
    }

    assert(!pthread_mutex_unlock(&timerq.mtx));
    return res;
}

/*
 * Mark for death the timer with thread_id in the timerq.
 */
int Mockit_timerq_delete(pthread_t thread_id){
    if (!(timerq.initialized & MTX_INITIALIZED)) return 1;
    assert(!pthread_mutex_lock(&timerq.mtx));

    printf("looking to delete timer id %lu\n", thread_id);
    if (queue_isempty(&timerq)) goto unlock;
   
    cbtimer_t *tmp = timerq.head;
    struct data *data = tmp->data;

    if (timerq.head == timerq.tail && data->thread_id__ == thread_id){ // one node only
        printf("... and deleting timer\n");
        timerq.head = timerq.tail = timerq.tail->next; //set head and tail to NULL
        free(tmp);
    }
    else{ // multiple nodes in the queue
        cbtimer_t *prev = tmp;
        tmp = tmp->next;

        for(;tmp; prev = tmp, tmp = tmp->next){
            data = tmp->data; 

            if (data->thread_id__ == thread_id){
                printf("deleting time2r");
                prev->next = tmp->next; 
                free(tmp);

                // if left with single node
                if (!timerq.head->next){
                    timerq.tail = timerq.head;
                }

                break;
            }
        }
    }

unlock: 
    assert(!pthread_mutex_unlock(&timerq.mtx));
    return 0;
}



