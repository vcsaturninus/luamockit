#include <errno.h>
#include <string.h>      // memset()
#include <time.h>        // clock_gettime(), clock_nanosleep() etc
#include <stdint.h>      // fixed width int types e.g. uint32_t
#include <stdbool.h>
#include <assert.h> 
#include <stdlib.h>      // free()
#include <signal.h>      // union sigval
#include <limits.h>
#include <stdio.h>

#include "mockit.h"

/*
 * Weirdly, the `struct timespec` passed to clock_nanosleep() stores
 * the nanoseconds (tv_nsec) in a long. However, clock_nanosleep() says it will 
 * return EINVAL if the the tv_nsec value it gets passed is > 999999999, which is 
 * in actuality MUCH less than MAX_LONG.
 */
#define MAX_NS_VAL 999999999

//===================================
// ---- File-scoped globals --------
//===================================

/* Unused in this compilation unit */
UNUSED(static pthread_mutex_t cbmtx) = PTHREAD_MUTEX_INITIALIZER;     // callback mutex


static void keep_timer_straight(struct timespec *ts, uint32_t secs, uint32_t nanosecs){
    (*ts).tv_sec += secs;
   
    /* the number of nanoseconds the way it's implemented in Mockit, will
       always be < 1 second. That is, it can/should be at most 999999999,
       (see MAX_NS_VAL defined above for this purpose) which is 1 nanosecond 
       from a full second.
    */
    assert(nanosecs < MAX_NS_VAL);

    /* If you go beyond MAX_NS_VAL, e.g. clock_nanosleep() WILL return an error.
       Therefore care must be taken we always stay within the range. If our value
       of nanoseconds > MAX_NS_VAL, we should convert it to seconds and put it in
       tv_sec, and only leave the remainder in tv_nsec.
    */
    long ns_free_space = MAX_NS_VAL - (*ts).tv_nsec;
    // there's enough space to accomodate nanosecs
    if (ns_free_space >= nanosecs){
        (*ts).tv_nsec += nanosecs;    
    }
    // not enough space: chop off the full second(s)
    else{
        nanosecs += (*ts).tv_nsec;
        uint32_t num_secs = nanosecs / NS_IN_SECS; 
        (*ts).tv_sec += num_secs;
        
        nanosecs = nanosecs % NS_IN_SECS;
        assert(nanosecs <= MAX_NS_VAL);
        (*ts).tv_nsec = nanosecs;
    }
}

//===================================
// ---- Function definitions  -------
//===================================

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
 * --> time_left 
 *     Time left (in milliseconds) until sleep woud've been completed. 
 *     0 if the sleep has been successfully completed. If do_restart
 *     is true, the sleep always completes and therefore time_left
 *     is always 0 in that case.
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
        *time_left = milliseconds;  // have not slept at all
        return -1;
    }

    // convert ms to secs and ns
    uint32_t  secs = milliseconds/ MS_IN_SECS; // 0 <= secs
    uint32_t  msecs = milliseconds % MS_IN_SECS;
    uint32_t  nsecs = msecs * NS_IN_MS; // time is given in ms but nanosleep expect ns

    // populate the struct timespect correctly, ensuring we stay within the
    // correct range for tv_nsec
    keep_timer_straight(&to_sleep, secs, nsecs);

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
                *time_left = milliseconds;
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
            *time_left = ms;
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
static void *sleeper__(void *ctx){
    struct data *dt = ctx;  

    //int res = Mockit_bsleep(dt->timeout, true, &remaining);
    assert(Mockit_bsleep(dt->timeout, true, NULL) == 0);

    // on sleep completion, call the callback
    dt->cb(ctx);

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

    if(pthread_create(&data->thread_id__, NULL, sleeper__, data)) return 1;
    if (pthread_detach(data->thread_id__)) return 1;
    
    return 0;
}

/*
 * Run on interval expiry of timers created via Mockit_getit()
 * as a thread start routine.
 * 
 * --> sv
 *     sv.sival_ptr can hold arbitrary data: in this particular
 *     case, it will always be a `struct data *` (see mockit.h for 
 *     the definition of this structure). This function will cast 
 *     sv.sival_ptr to a `struct data *` and then call the cb() 
 *     stored there.
 */
static void threadf(union sigval sv){
    struct data *dt = sv.sival_ptr;
    dt->cb(dt);
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
    assert(data);
    data->is_cyclic__ = true; 

    struct sigevent sev;         // used to specify notification via signal or thread for interval timers
    struct itimerspec timerspec; // the actual timer object

    uint32_t  secs, msecs, nsecs;
    secs = interval / MS_IN_SECS;
    msecs = interval % MS_IN_SECS;
    nsecs = msecs * NS_IN_MS;

    timerspec.it_value.tv_sec = secs;
    timerspec.it_value.tv_nsec = nsecs;   // first expire interval*FACTOR ns from now
    timerspec.it_interval.tv_sec = secs;  // keep expiring every INTERVAL seconds
    timerspec.it_interval.tv_nsec = nsecs;

    sev.sigev_notify = SIGEV_THREAD;       // notify via thread
    sev.sigev_notify_function = threadf;   // on each timer expiration call this function as if it were a thread entry function
    sev.sigev_notify_attributes = NULL;    // use default attributes
    sev.sigev_value.sival_ptr = data;      // call threadf() with data as param
    
    if(timer_create(CLOCK_REALTIME, &sev, &(data->timer_id__)) == -1){
        return errno;
    }

    // ARM the timer based on the settings in timerspec, and save timer id
    if (timer_settime(data->timer_id__, 0, &timerspec, NULL) == -1){ 
        return errno;
    }
    
    return 0; // success
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
 */
int Mockit_destroy(struct data *dt, bool free_data, bool free_ctx){
    assert(dt);

    // for one-off timers the dt->timer_id will be invalid, since it was never set.
    if (dt->is_cyclic__){  // interval timer, not one-shot timer
        if (timer_delete(dt->timer_id__) == -1){
            return errno;
        }
    }
    
    if (free_ctx) free(dt->ctx);

    if (free_data){
        free(dt);
    }
    else{
        memset(dt, 0, sizeof(struct data));
    }
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
                                      void *ctx)
{
    struct data *dt = calloc(1,sizeof(struct data));
    assert(dt);
    assert(cb);

    dt->cb = cb;
    dt->timeout = timeout;
    dt->ctx = ctx;

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
                                     void *ctx)
{
    assert(dt);
    assert(cb);

    memset(dt, 0, sizeof(struct data));

    dt->cb = cb;
    dt->timeout = timeout;
    dt->ctx = ctx;
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

