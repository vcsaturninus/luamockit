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
#include "common.h"

#ifdef DEBUG_MODE
extern int DEBUG;
#endif

/*
 * Weirdly, the `struct timespec` passed to clock_nanosleep() stores
 * the nanoseconds (tv_nsec) in a long. However, clock_nanosleep() says it will
 * return EINVAL if the the tv_nsec value it gets passed is > 999999999, which is
 * in actuality MUCH less than MAX_LONG. So MAX_LONG cannot be used to check
 * that the tv_nsec Mockit has set is within range: a new constant is instead necessary.
 */
#define MAX_NS_VAL 999999999L

/* MFD is set to mark a timer as disarmed; MOD is setw
 * in acknowledgement by the timer thread before thread exit */
#define MOCKIT_MFD   0x1
#define MOCKIT_MOD   0x2


//============================
// ---- Type definitions -----
//============================

/* callback function type */
typedef void (* timedcb)(void *);


//===================================
// ---- Function definitions  -------
//===================================

/*
 * Check if the given mark has MOD set.
 *
 * mark must normally correspond to a mockit->mark__.
 */
bool Mockit_hasmod(uint8_t mark){
    return (mark & MOCKIT_MOD) ? true : false;
}

/*
 * Set MOD - the mark of destruction - in mark and return it */
uint8_t Mockit_setmod(uint8_t mark){
    return (mark |= MOCKIT_MOD);
}

/*
 * Set MFD - mark for destruction - in mark and return it. */
uint8_t Mockit_setmfd(uint8_t mark){
    return (mark |= MOCKIT_MFD);
}

/*
 * Check if the given mark has MFD set.
 *
 * mark must normally correspond to a mockit->mark__.
 */
bool Mockit_ismfd(uint8_t mark){
    return (mark & MOCKIT_MFD) ? true : false;
}

/*
 * Mark for destruction the timer with thread_id.
 *
 * This will mark the timer object with MFD to let it knoww
 * to terminate. The timer will set MOD in acknowledgementw
 * once it's seen the mark. The user must always register aw
 * destructor so normally MOD is not and cannot used for anything.w
 * However, the user _could_ conceivably check for this mark inw
 * the destructor and perhaps delay the destruction or offloadw
 * it onto another function etc.
 *
 * FMI, see the notes at the top where MOCKIT_MOD and MOCKIT_MFD are defined.
 */
void Mockit_disarm(struct mockit *timer){
    say(DEBUG, "disarming -- seting mfd\n");
    timer->mark__ = Mockit_setmfd(timer->mark__);
}
/*
 * Return a time tuple of (seconds, milliseconds).
 *
 * <-- secs, @out
 *     Number of seconds since the epoch (i.e. a Unix timestamp).
 *
 * <-- ms, @out
 *     millieseconds since the last second.
 *
 * <-- return
 *     0 on success, the value of errno on error as set
 *     by clock_gettime() on error
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
 * However it is sometimes the case that millisecondw
 * precision is needed instead.
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

    *timestamp = secs * MS_IN_SECS + msecs;

    return 0;
}

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
 * structure such that the correct range is not overshot.
 *
 * FMI see MAX_NS_VAL above and the comments below in the body of the function.
 */
void timespec_add_ms__(struct timespec *ts, uint32_t milliseconds){
    assert(ts);

    say(DEBUG, "%s(), 0) => secs=%lu, nsecs=%lu \n", __func__, (*ts).tv_sec, (*ts).tv_nsec);

    // convert ms to secs and ns
    uint32_t  secs = milliseconds/ MS_IN_SECS; // sec >= 0; (0 if milliseconds < 1000)
    uint32_t  msecs = milliseconds % MS_IN_SECS;
    uint32_t  nsecs = msecs * NS_IN_MS; // time is given in ms but nanosleep expects ns
                                        //
    say(DEBUG, "%s(), 1): secs=%u, msecs=%u,nsecs=%u\n", __func__, secs, msecs, nsecs);

    (*ts).tv_sec += secs;

    /* the number of nanoseconds the way it's implemented in Mockit, will
       always be < 1s. That is, it can/should be at most 999999999,
       (see MAX_NS_VAL defined above for this purpose) which is 1 nanosecond
       from a full second. */
    assert(nsecs < MAX_NS_VAL);

    /* If you go beyond MAX_NS_VAL, e.g. clock_nanosleep() WILL return an error.
       Therefore care must be taken we always stay within the range. If our value
       of nanoseconds > MAX_NS_VAL, we should convert it to seconds and put it in
       tv_sec, and only leave the remainder in tv_nsec. */
    long ns_free_space = MAX_NS_VAL - (*ts).tv_nsec;
    // there's enough space to accomodate nsecs
    if (ns_free_space >= nsecs){
        (*ts).tv_nsec += nsecs;
    }

    // not enough space: chop off the full second
    else{
        nsecs += (*ts).tv_nsec;
        uint8_t num_secs = nsecs / NS_IN_SECS;  /* can only be < 2s */
        (*ts).tv_sec += num_secs;

        nsecs = nsecs % NS_IN_SECS;
        assert(nsecs <= MAX_NS_VAL);
        (*ts).tv_nsec = nsecs;
    }
    say(DEBUG, "%s(), 2) => secs=%lu, nsecs=%lu \n", __func__, (*ts).tv_sec, (*ts).tv_nsec);
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
 * --> time_left, @opt @out
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
 *     0 on sucess;
 *     1 if clock_gettime() failed, else the error code returned by
 *     clock_nanosleep().
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
 * so this function is not portable across unices. However, it is used here
 * instead of nanosleep() because it avoids time drift (possible when
 * repeatedly restarting sleep after signal interrupts) by allowing the
 * specification of an absolute time value rather than a relative duration.
 */
int Mockit_bsleep(uint32_t milliseconds, bool do_restart, uint32_t *time_left){
    int res = 0;
    struct timespec to_sleep; // current time + milliseconds
    memset(&to_sleep, 0, sizeof(struct timespec));

    // 1)
    // do resume sleep when interrupted by signals => time_left unused
    if (do_restart){

        if (clock_gettime(MOCKIT_CLOCK_ID, &to_sleep) == -1){
            say(DEBUG, "clock_gettime failed\n");
            if (time_left) *time_left = milliseconds;  // have not slept at all
            return 1;
        }

        // populate the `struct timespec` correctly, ensuring we stay within the
        // correct range for tv_nsec
        timespec_add_ms__(&to_sleep, milliseconds);

       // rem is not used when TIMER_ABSTIME is used
        res = clock_nanosleep(MOCKIT_CLOCK_ID, TIMER_ABSTIME, &to_sleep, NULL);

        if (res){
            if (res == EINTR){
                while (res == EINTR){
                    res = clock_nanosleep(MOCKIT_CLOCK_ID, TIMER_ABSTIME, &to_sleep, NULL);
                }
            }
            else{
                if (time_left) *time_left = milliseconds;
            }
        }
    }

    // 2)
    // do NOT restart sleep when interrupted by signals => time_left is used
    // use relative duration not absolute time value
    else{
        struct timespec rem;
        memset(&rem, 0, sizeof(rem));
        timespec_add_ms__(&to_sleep, milliseconds);

        res = clock_nanosleep(MOCKIT_CLOCK_ID, 0, &to_sleep, &rem);

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
 * Sleep for mockit->timeout milliseconds before calling mockit->cb().
 * This function is used for one-shot and interval timers.
 *
 * This function is called to sleep in a different thread separate
 * from the main one so that the library isn't blocked by the sleep.
 * If interrupted by a signal, it will resume sleeping until done.
 *
 * mockit->cb MUST be threadsafe or reentrant; it's the responsibility
 * of the library user to ensure that's the case.
 *
 * <-- return
 *     NULL is always returned. The function simply has this signature
 *     because it's required by pthreads of thread-starting functions.
 */

static void *oneshot__(void *mockit){
    assert(!pthread_detach(pthread_self()));

    say(DEBUG, "entering oneshot__ thread\n");

    /* one-off timers sleep ONCE for mockit->timeout time
     * then call mockit->cb and then self-destruct. */
    struct mockit *timer = mockit;
    assert(Mockit_bsleep(timer->timeout__, true, NULL) == 0);
    if (Mockit_ismfd(timer->mark__)){
        say(DEBUG, "oneshot__: not calling cb because timer has been disarmed\n");
    }else{
        timer->cb(timer); /* run if not disarmed */
    }

    if (timer->destructor){
        say(DEBUG, "oneshot__: calling destructor\n");
        timer->destructor(timer);
        say(DEBUG, "oneshot__: after destructor\n");
    }
    else{
        if (timer->free_mem__) free(timer);
    }

    say(DEBUG, "oneshot__: calling pthread_exit()\n");

    pthread_exit(NULL);
    return NULL;
}

/*
 * Every interval timer is simply a thread that runs this function until expiry.
 *
 * Interval timers:
 * 1) sleep for for timer->timeout,
 * 2) call timer->cb,
 * 3) IFF the associated mockit mark has mfd set, they self-destruct
 * 4) otherwise they go back to 1).
 *
 * <-- return
 *     This function returns nothing. The `void *` is simply the signature expected
 *     by pthread of thread-entry functions.
 */
static void *cycle__(void *mockit){
    struct mockit *timer = mockit;
    bool mfd = false;
    uint32_t interval = timer->timeout__;
    timedcb callback = timer->cb;

/* No locking is needed when checking mfd because
 * the thread never sets it, only checks it. If the
 * caller sets it at any point, it will be caught either
 * on this or the next loop pass. The thread then acknolwedges
 * that by setting MOD.w
 * sets MOD IFF MFD has been set.  */
while (!mfd){
    assert(Mockit_bsleep(interval, true, NULL) == 0);
    say(DEBUG, "calling callback with timer=%p, mark=%u, ival=%u\n", (void *)timer, timer->mark__, timer->timeout__);
    callback(timer);

    mfd = Mockit_ismfd(timer->mark__);
}
    // otherwise, timer has been marked for destruction; acknowledgew
    // this by setting the 'mark of destruction',  (MOCKIT_MOD)w
    // and free the memory if no destructor has been provided
    timer->mark__ = Mockit_setmod(timer->mark__);  /* set mark of destruction */
    say(DEBUG, "shutting down thread: set mark to %i from %i, mark & MOD = %i\n", timer->mark__, timer->mark__, timer->mark__ & MOCKIT_MOD);

    int *rc = calloc(1, sizeof(int));
    if (timer->destructor){
        say(DEBUG, "cycle__: calling destructor\n");
        timer->destructor(mockit);
        say(DEBUG, "cycle__: after destructor call\n");
    }
    else{
        if (timer->free_mem__) free(timer);
    }

    pthread_exit(rc);
}

/*
 * Call a callback ONCE on timer expiry.
 *
 * --> time
 *     time, in milliseconds, after which to call timer->cb.
 *
 * --> timerw
 *     a pointer to an initialized `struct mockit`;
 *
 * --> timer->cb
 *     a callback function that returns a void pointer and takes
 *     a void pointer; mockit WILL always call the callback with a
 *     `struct mockit` pointer so it must be cast and interpreted as appropriate.
 *     The caller must ensure the callback is reentrant or otherwise
 *     thread-safe.
 *
 * <-- return
 *     1 on failure, 0 on success;
 */
static inline int Mockit_oneoff(struct mockit *timer){
    if (!timer) return -1;

    // create a thread that will start life by calling oneshot__; this
    // will sleep for TIME duration before calling timer->cb() with
    // 'timer' as an argument; the thread will store its id in thread_id and will
    // use default thread attributes.
    timer->mark__ = 0;
    say(DEBUG, "--oneoff will sleep for %u ms\n", timer->timeout__);
    if (pthread_create(&timer->thread_id__, NULL, oneshot__, timer)) return 1;

    return 0;
}

/*
 * Arrange for timer->cb() to be called on interval expiry.
 *
 * --> interval
 *     the value of the interval, in milliseconds, at which the timer
 *     is to expire and the callback in timer->cb is to be called.
 *
 * --> timerw
 *     an initialized `struct mockit *` (see definition in mockit.h).
 *     The timer id of the interval timer created is written to timer->timer_id__.
 *     The caller can destroy the associated interval timer by callingw
 *     Mockit_destroy() on this id (see Mockit_destroy()).
 *     On failure, timer is set to NULL.
 *
 * <-- return
 *     0 on success, and the value of errno on error/failure.
 */
static inline int Mockit_getit(struct mockit *timer){
    assert(timer);

    // create a thread that will start life by calling cycle__;
    // store the thread id in thread_id and use default thread attributes
    if(pthread_create(&timer->thread_id__, NULL, cycle__, timer)){
        return 1;
    }

    return 0;
}

/*
 * Destroy specified interval or disarm one-off timer.w
 *
 * One-off timers can be disarmed, though they're are inherentlyw
 * self-disarming. Normally a one-off timer sleeps once for thew
 * set duration, wakes up, calls the associated callback, thenw
 * terminates. If the sleep duration is long the user may decide
 * to disarm the callback while it's still asleep. In that case
 * it will not call the associated callback on wakeup as it has
 * already been disarmed. If called on a one-off timer, thisw
 * function is completely synonymous with `Mockit_disarm()`.
 *
 * For interval timers, his function will first disarm the timer,w
 * then wait for it to either perform default cleanup or call aw
 * registered destructor, and then _join_ the thread when it exits.
 * This ensures all resources, including thread ones, are released.
 *
 * --> timerw
 *     fully initialized and armed interval or one-off timer objectw
 *     to be destroyed.w
 *
 * <-- return
 *     always 0 for one-off timers; for interval timers, it will bew
 *     the error code returned by `pthread_join()` (if failed) or thew
 *     return code passed by the exiting timer thread to `pthread_exit()`
 *     obtained by this function via `pthread_join()`.
 */
int Mockit_destroy(struct mockit *timer){
    assert(timer);
    int rc = 0;
    void *threadrc_ptr = NULL;  /* must be cast to int* */
    int threadrc = 0;
    int cyclic = timer->is_cyclic__;

    Mockit_disarm(timer);

    /* no joining for one-off timers; synonymous with Mockit_disarm() */
    if (!cyclic) return 0;

    /* interval timers must be joined */
    say(DEBUG, "waiting for thread to join; join returned %i, pthread_exit=%i\n", rc, threadrc);
    rc = pthread_join(timer->thread_id__, &threadrc_ptr);
    if (threadrc_ptr){
        threadrc = *(int *)threadrc_ptr;
        free(threadrc_ptr);
    }

    if (rc) return 1;
    if (threadrc) return 2;
    return 0;
}

/*
 * Dynamically allocate a timer object and initialize it.
 *
 * The client may call this to get a timer to pass to
 * Mockit_oneoff() and Mockit_getit().
 *
 * --> cb
 *     a callback with the specified signature that is to be called on
 *     on expiry of one-shot or interval timers with a pointer to the
 *     associated timer object as the sole parameter.
 *
 * --> timeout
 *     The duration (either for a one-shot timer or for an interval timer)
 *     to wait before calling the callback.
 *
 * --> cyclic
 *     true if this timer is meant to be an interval timer; false for a one-off
 *     timer
 *
 * --> ctx
 *     The callback function will be passed this as a param when called.
 *     It's for arbitrary use by the user. If used and if dynamically allocated
 *     by the user or if requiring certain resource cleanup, the user should
 *     also provide a destructor function.
 *
 * --> destructor
 *     Optional destructor to be called before thread exit. If NULL, the thread
 *     function will perform some default cleanup (simply call `free()` on the
 *     timer object if dynamically allocated, else do nothing). Otherwise if
 *     a destructor is specified, it will be called with a pointer to the timer
 *     object as the sole parameter.
 *     The destructor should free BOTH the timer object AND the `ctx` field if used
 *     and if appropriate (see previous point).
 *
 * <-- return
 *     Dynamically allocated and initialized timer object.
 */
struct mockit *Mockit_dynamic_init(
                                    void (*cb)(void *timer),
                                    uint32_t timeout,
                                    bool cyclic,
                                    void *ctx,
                                    int (*destructor)(void *timer)
                             )
{
    struct mockit *timer = calloc(1,sizeof(struct mockit));
    assert(timer);
    assert(cb);

    timer->cb = cb;
    timer->timeout__ = timeout;
    timer->destructor = destructor;
    timer->ctx = ctx;
    timer->free_mem__ = 1;
    timer->mark__ = 0;
    timer->is_cyclic__ = (cyclic) ? 1 : 0;

    return timer;
}

/*
 * Initialize statically allocated timer object.
 *
 * See `Mockit_dynamic_init()` for an explanation of each
 * expected parameter.w
 * The only parameter that differs is the destructor; if provided,
 * it should only `free()` the `ctx` field if appropriate, butw
 * _not_ the timer object itself, since it has _not_ been dynamically
 * allocated.
 */
void Mockit_static_init(
                        struct mockit *timer,
                        void (*cb)(void *timer),
                        uint32_t timeout,
                        bool cyclic,
                        void *ctx,
                        int (*destructor)(void *ctx)
                        )
{
    assert(timer);
    assert(cb);
    memset(timer, 0, sizeof(struct mockit));

    timer->ctx = ctx;
    timer->cb = cb;
    timer->timeout__ = timeout;
    timer->destructor = destructor;
    timer->is_cyclic__ = (cyclic) ? 1 : 0;
}

/*
 * Arm a fully-initialized one-off or interval timer.
 *
 * Arming a timer creates a new thread appropriate forw
 * the type of timer and the timer is now considered tow
 * be active. On timer expiry the associated callback
 * will be called.
 */
int Mockit_arm(struct mockit *timer){
    assert(timer);

    if(timer->is_cyclic__){
        return Mockit_getit(timer);
    }
    else{
        return Mockit_oneoff(timer);
    }
}


