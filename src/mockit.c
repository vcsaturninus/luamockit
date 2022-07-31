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

/*
 * Weirdly, the `struct timespec` passed to clock_nanosleep() stores
 * the nanoseconds (tv_nsec) in a long. However, clock_nanosleep() says it will
 * return EINVAL if the the tv_nsec value it gets passed is > 999999999, which is
 * in actuality MUCH less than MAX_LONG. So MAX_LONG cannot be used to check
 * that the tv_nsec Mockit has set is within range: a new constant is instead necessary.
 */
#define MAX_NS_VAL 999999999L

/*
 * When Mockit_disarm() is called, MOCKIT_MFD ('marked for destruction')
 * is set into data->mark__. When the thread responsible for the interval
 * timer notices this (which it checks on every interval expiration),
 * it will acknowledge it by setting MOCKIT_MOD ('mark of destruction')
 * into data->mark__. After setting this, the thread exits and discontinues
 * the timer i.e. the associated callback no longer gets called as a result
 * of this particular timer (though it may be called as a result of other
 * timers if it's so registered).
 *
 * This mechanism is needed if the thread is not in charge of freeing memory:
 * when the caller disarms a timer, it can then check if the thread has acknowledged
 * the disarming before actually freeing the memory. If the caller does not wait
 * for the acknowledgement then the thread is likely to commit 'use after free'
 * by dereferencing the now freed `struct mockit`, leading to memory corruption
 * or a crash.
 * Converesely, if the thread IS in charge of freeing the memory, then once the
 * caller has set MFD (by calling Mockit_disarm()), it should NOT wait for
 * an acknowledgement: the thread may have already freed the mockit object
 * (struct data) so the _caller_ is in this scenario at risk of committing
 * 'use after free' itself! Any reference to the mockit object should be set to NULL
 * by the caller and no longer used to access that timer.
 *
 * The calls to Mockit_disarm() and Mockit_mod() always succeed (with the notes
 * mentioned above).
 *
 * See the comments on Mockit_disarm() and Mockit_mod() fmi.
 */
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
 * Set MOD - the mark of destruction - on mark and return it.
 */
uint8_t Mockit_setmod(uint8_t mark){
    return (mark |= MOCKIT_MOD);
}

/*
 * Set MFD - mark for destruction - on mark and return it.
 */
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
 * If the caller is responsible for freeing the `struct mockit` i.e.
 * `Mockit_{static,dynamic}_data_init()` was NOT called with self_destruct=true,
 * it should wait until  the thread responsible for the interval timer has
 * acknowledged this, which can be checked with Mockit_mod(). Only then is it safe
 * for the caller to free() the `struct mockit *` (and mockit->ctx, if used).
 *
 * Otherwise if the caller is not responsible for freeing this data, it should
 * consider the mockit object and the ctx desotroyed and no longer use them for
 * anything (e.g. it's illegal to derefence these, call Mockit_mod() on the mockit
 * object,  etc.) once Mockit_disarm() was called.
 *
 * FMI, see the notes at the top where MOCKIT_MOD and MOCKIT_MFD are defined.
 */
void Mockit_disarm(struct mockit *mit){
    printf("disarming -- seting mfd\n");
    mit->mark__ = Mockit_setmfd(mit->mark__);
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

    // convert ms to secs and ns
    uint32_t  secs = milliseconds/ MS_IN_SECS; // sec >= 0; (0 if milliseconds < 1000)
    uint32_t  msecs = milliseconds % MS_IN_SECS;
    uint32_t  nsecs = msecs * NS_IN_MS; // time is given in ms but nanosleep expects ns

    (*ts).tv_sec += secs;

    /* the number of nanoseconds the way it's implemented in Mockit, will
       always be < 1s. That is, it can/should be at most 999999999,
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
    // not enough space: chop off the full second
    else{
        nsecs += (*ts).tv_nsec;
        //todo remove check
        assert(nsecs < NS_IN_SECS<<1);
        uint8_t num_secs = nsecs / NS_IN_SECS;  /* can only be < 2s */
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

    if (clock_gettime(MOCKIT_CLOCK_ID, &to_sleep) == -1){
        printf("clock_gettime failed\n");
        if (time_left) *time_left = milliseconds;  // have not slept at all
        return 1;
    }

    // populate the `struct timespec` correctly, ensuring we stay within the
    // correct range for tv_nsec
    timespec_add_ms__(&to_sleep, milliseconds);

    // 1)
    // do resume sleep when interrupted by signals => time_left unused
    if (do_restart){
        // rem is not used when TIMER_ABSTIME is used
        res = clock_nanosleep(MOCKIT_CLOCK_ID, TIMER_ABSTIME, &to_sleep, NULL);
        //printf("clock nanosleep returned res=%i\n", res);
        //printf("ENOTSUP is %i\n", ENOTSUP);

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
    printf("oneshot called\n");
    /* one-off timers sleep ONCE for mockit->timeout time
     * then call mockit->cb and then self-destruct. */
    struct mockit *mit = mockit;
    assert(Mockit_bsleep(mit->timeout__, true, NULL) == 0);
    mit->cb(mit);

    if (mit->self_destr__){
        if (mit->destructor){
            puts("in destructor, calling destructor");
            mit->destructor(mockit);
            puts("after destructor");
        }
        else{
            if (mit->free_mem__) free(mit);
        }
    }

    puts("calling pthread_exit()");

    pthread_exit(NULL);
    return NULL;
}

/*
 * Every interval timer is simply a thread that runs this function until expiry.
 *
 * Interval timers:
 * 1) sleep for for dt->timeout,
 * 2) call dt->cb,
 * 3) IFF the associated mockit mark has mod AND mfd set, they self-destruct,
 * 4) otherwise they go back to 1).
 *
 * <-- return
 *     This function returns nothing. The `void *` is simply the signature expected
 *     by pthread of thread-entry functions.
 */
static void *cycle__(void *mockit){
    struct mockit *mit = mockit;
    bool mfd = false;
    uint32_t interval = mit->timeout__;
    timedcb callback = mit->cb;

/* No locking is needed when checking mfd because
 * the thread never sets it, only checks it. If the
 * caller sets it at any point, it will be caught either
 * on this or the next loop pass. The thread then acknolwedges
 * that by setting MOD. Simiarly no locking is required,
 * because the thread only sets it and the caller only ever
 * checks it. Further, the caller only sets MFD ONCE
 * and the thread ONLY sets MOD IFF MFD has been set.
 * By definition therefore these cannot be performed at
 * the same time and there's therefore no race condition
 * that must be protected against.
 */
while (!mfd){
    assert(Mockit_bsleep(interval, true, NULL) == 0);
    printf("calling callback with mit=%p, mark=%u, ival=%u\n", (void *)mit, mit->mark__, mit->timeout__);
    callback(mit);

    mfd = Mockit_ismfd(mit->mark__);
}
    //puts("got marked for destruction: will delete and destroy");
    // otherwise, timer has been marked for destruction;
    // acknowledge this by setting the 'mark of destruction',
    // (MOCKIT_MOD) and free the memory if responsible.
    unsigned int mark = mit->mark__;
    mit->mark__ = Mockit_setmod(mit->mark__);  /* set mark of destruction */
    fprintf(stderr, "shutting down thread: set mark to %i from %i, mark & MOD = %i\n", mit->mark__, mark, mit->mark__ & MOCKIT_MOD);

    // clearly, the caller can and should only check the MOD
    // IFF the thread is NOT responsible for freeing the resources i.e.
    // this function does NOT free data.
    int *rc = calloc(1, sizeof(int));
    if (mit->self_destr__){
        if (mit->destructor){
            puts("---- in destructor, calling destructor");
            mit->destructor(mockit);
            puts("---- after destructor");
        }
        else{
            if (mit->free_mem__) free(mit);
        }
    }
    pthread_exit(rc);
}

/*
 * Call a callback ONCE on timer expiration.
 *
 * --> time
 *     time, in milliseconds, after which to call data->cb.
 *
 * --> data
 *     a pointer to an initialized `struct mockit`;
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
int Mockit_oneoff(struct mockit *mit){
    if (!mit) return -1;   // data should've been allocated and partly populated by caller

    // create a thread that will start life by calling sleeper__; this
    // will sleep for TIME duration before calling data->cb() with
    // 'data' as an argument; the thread will store its id in thread_id and will
    // use default thread attributes.
    mit->mark__ = 0;
    printf("--oneoff will sleep for %u ms\n", mit->timeout__);
    if (pthread_create(&mit->thread_id__, NULL, oneshot__, mit)) return 1;

    return 0;
}

/*
 * Arrange for data->cb() to be called on interval expiration.
 *
 * --> interval
 *     the value of the interval, in milliseconds, at which the timer
 *     is to expire and the callback in data->cb is to be called.
 *
 * --> data
 *     an initialized `struct mockit *` (see definition in mockit.h).
 *     DATA is further manipulated and populated by this function before
 *     being returned to the caller - on success.
 *     The timer id of the interval timer created is written to data->timer_id__.
 *     The caller can destroy the associated interval timer by calling Mockit_destroy()
 *     on this id (see Mockit_destroy()).
 *     On failure, data is set to NULL.
 *
 * <-- return
 *     0 on success, and the value of errno on error/failure.
 */
int Mockit_getit(struct mockit *mit){
    assert(mit);

    // create a thread that will start life by calling cycle__;
    // store the thread id in thread_id and use default thread attributes
    if(pthread_create(&mit->thread_id__, NULL, cycle__, mit)){
        return 1;
    }

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
 *
 * timeout of -1 means wait indefinitely until thread terminates
 * (pthread_join). Otherwise it should be a timeout in ms: if the
 * thread has not terminated in that time frame, give up on destroying it:
 * note this means its resources may not be released.
 *
 * A timeout of zero means that the timer is disarmed but not waited
 * for its actual destruction.
 */
int Mockit_destroy(struct mockit *mit, int32_t timeout){
    assert(mit);
    int rc = 0;
    void *threadrc_ptr = NULL;  /* must be cast to int* */
    int threadrc = 0;
    int self_destroy = mit->self_destr__;

    Mockit_disarm(mit);

    if (timeout == -1){ /* wait for thread to positively join (terminate) */
        rc = pthread_join(mit->thread_id__, &threadrc_ptr);
        if (threadrc_ptr){
            threadrc = *(int *)threadrc_ptr;
            free(threadrc_ptr);
        }

        if (rc) return 1;

        /* disregard valgrind warning about 'uninitialized value': this is set by pthread_join() */
        if (threadrc) return 2;

        printf("waiting for thread to join; join returned %i, pthread_exit=%i\n", rc, threadrc);

        if (!self_destroy){
            free(mit->ctx);
            if (mit->free_mem__) free(mit);
        }
    }

    else if (timeout > 0){
        assert(!Mockit_bsleep(timeout, true, NULL));
        if (!Mockit_hasmod(mit->mark__)) return -1;

        if (!self_destroy){
            free(mit->ctx);
            if (mit->free_mem__) free(mit);
        }
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
struct mockit *Mockit_dynamic_init(
                                    void (*cb)(void *mockit),
                                    uint32_t timeout,
                                    bool cyclic,
                                    void *ctx,
                                    bool self_destruct,
                                    int (*destructor)(void *ctx)
                             )
{
    struct mockit *dt = calloc(1,sizeof(struct mockit));
    assert(dt);
    assert(cb);

    dt->cb = cb;
    dt->timeout__ = timeout;
    dt->destructor = destructor;
    dt->ctx = ctx;
    dt->self_destr__ = (self_destruct) ? 1 : 0;
    dt->free_mem__ = 1;
    dt->mark__ = 0;
    dt->is_cyclic__ = (cyclic) ? 1 : 0;

    return dt;
}

/*
 * Initialize a statically declared struct data.
 *
 * See Mockit_dynamic_data_init fmi.
 */
void Mockit_static_init(
                        struct mockit *mit,
                        void (*cb)(void *mit),
                        uint32_t timeout,
                        bool cyclic,
                        void *ctx,
                        bool self_destruct,
                        int (*destructor)(void *ctx)
                        )
{
    assert(mit);
    assert(cb);
    memset(mit, 0, sizeof(struct mockit));

    mit->ctx = ctx;
    mit->cb = cb;
    mit->timeout__ = timeout;
    mit->destructor = destructor;
    mit->is_cyclic__ = (cyclic) ? 1 : 0;
    mit->self_destr__ = (self_destruct) ? 1 : 0;
}



int Mockit_arm(struct mockit *mit){
    assert(mit);

    if(mit->is_cyclic__){
        return Mockit_getit(mit);
    }
    else{
        int rc = Mockit_oneoff(mit);
        printf("returning rc=%i\n", rc);
        //return Mockit_oneoff(mit);
        return rc;
    }
}


