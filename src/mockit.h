#ifndef MOCKIT_H__
#define MOCKIT_H__

/* ===============================================================================*\
 |  BSD 2-Clause License                                                           |
 |                                                                                 |
 |  Copyright (c) 2022, vcsaturninus -- vcsaturninus@protonmail.com                |
 |  All rights reserved.                                                           |
 |                                                                                 |
 |  Redistribution and use in source and binary forms, with or without             |
 |  modification, are permitted provided that the following conditions are met:    |
 |                                                                                 |
 |  1. Redistributions of source code must retain the above copyright notice, this |
 |     list of conditions and the following disclaimer.                            |
 |                                                                                 |
 |  2. Redistributions in binary form must reproduce the above copyright notice,   |
 |     this list of conditions and the following disclaimer in the documentation   |
 |     and/or other materials provided with the distribution.                      |
 |                                                                                 |
 |  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"    |
 |  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE      |
 |  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE |
 |  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE   |
 |  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL     |
 |  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR     |
 |  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER     |
 |  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  |
 |  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE  |
 |  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.           |
 \*===============================================================================*/

#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>

/* suppress warnings about variable being declared but unused */
#if defined(__GNUC__) || defined(__GNUG__) || defined(__clang__)
# define UNUSED(x) x __attribute__((unused))
#else
# define UNUSED(x) do { (void)(x); } while (0)
#endif


/*
 * The mockit library takes from lua or C time values in MILLISECONDS but in fact works
 * on seconds and nanosecond values (e.g. for example nanosleep() expects seconds and nanoseconds).
 * These constants are used for conversion between seconds, miliseconds, and nanoseconds.
 */
#define NS_IN_SECS 1000000000LU   // 10^9
#define MS_IN_SECS 1000U          // 10^3
#define NS_IN_MS   1000000LU      // 10^6

/* The CLOCK type to use with clock_naonosleep(), clock_gettime() etc;
 * alternative options are commented out */
#define MOCKIT_CLOCK_ID    CLOCK_BOOTTIME
//#define MOCKIT_CLOCK_ID    CLOCK_MONOTONIC_RAW
//#define MOCKIT_CLOCK_ID    CLOCK_MONOTONIC



/*
 * __ -postfixed members are set by the library functions and should not
 * be modified by the user other than via the init function.
 * The `ctx` field is reserved for arbitrary use by the user. If used,
 * the user should provide a destructor function that knows how free
 * it as well if necessary.
 */
struct mockit{
    pthread_t thread_id__;    // thread_id for timer thread created for each one-shot or interval timer
    uint8_t is_cyclic__  : 1, // flag to mark interval timers; these are destroyed differently from one-off timers
            mark__       : 2, // used to communicate the destruction state
            free_mem__   : 1; // used to let the timer thread know (not) to call free() on the `struct mockit`
    void   *ctx;              // for arbitrary use by the user; if used, destructor should be provided as well
    uint32_t timeout__;       // number of milliseconds before (one-off or interval) callback gets called
    void (*cb)(void *mockit); // function to call on expiry of one-off or interval timer
    int (*destructor)(void *ctx); // optional function to call on timer termination to release resources
};

int Mockit_gettime(time_t *secs, long *ms);

int Mockit_mstimestamp(uint64_t *timestamp);

int Mockit_bsleep(uint32_t milliseconds, bool do_restart, uint32_t *time_left);

void Mockit_static_init(
                             struct mockit *mit,
                             void (*cb)(void *mockit),
                             uint32_t timeout,
                             bool cyclic,
                             void *ctx,
                             int (*destructor)(void *mockit)
                             );

struct mockit *Mockit_dynamic_init(
                             void (*cb)(void *mockit),
                             uint32_t timeout,
                             bool cyclic,
                             void *ctx,
                             int (*destructor)(void *mockit)
                             );

int Mockit_arm(struct mockit *mit);
void Mockit_disarm(struct mockit *mit);
int Mockit_destroy(struct mockit *dt);

#endif
