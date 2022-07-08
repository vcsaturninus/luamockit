#ifndef MOCKIT_H__
#define MOCKIT_H__

/*  ******************************************************************************
 *  BSD 2-Clause License
 *  
 *  Copyright (c) 2022, vcsaturninus -- vcsaturninus@protonmail.com
 *  All rights reserved.
 *  
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  
 *  1. Redistributions of source code must retain the above copyright notice, this
 *     list of conditions and the following disclaimer.
 *  
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *  ******************************************************************************
 */

#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>

#include "queue.h"

// suppress warning about variable being declared but not used
#if defined(__GNUC__)
# define UNUSED(x) x __attribute__((unused))
#else
# warning("Compiler is not gcc so UNUSED is undefined!")
# define UNUSED 
#endif


/* 
 * The mockit library takes from lua or C time values in MILLISECONDS but in fact works 
 * on seconds and nanosecond values (e.g. for example nanosleep() expects seconds and nanoseconds).
 * These constants are used for conversion between seconds, miliseconds, and nanoseconds.
 */
#define NS_IN_SECS 1000000000   // 10^9
#define MS_IN_SECS 1000         // 10^3
#define NS_IN_MS   1000000      // 10^6

#define INT_REF_COUNT 3         // number of int references that a `struct data`'s `refs` array member holds.


/* __ -postfixed members are set by the library functions and should not 
 * be touched by the user.
 */
struct data{
    pthread_t  thread_id__;  // thread_id for sleeper thread created for each one-shot timer; populated by lib function
    bool       is_cyclic__;  // flag to mark interval timers: only call timer_delete on timer_id if this is true (else segfaults)
    bool mfd;          // marked for death
    bool enqueued;     // added to timerq 
    bool free_data;    // whether to free the whole struct data when destroying
    bool free_ctx;     // whether to free dt->ctx when destroying
    // for arbitrary use by the caller; e.g. an int here could be an index in the lua registry e.g. to a lua callback 
    int refs[INT_REF_COUNT];        
    uint32_t timeout;        // number of milliseconds before (one-off or interval) callback gets called 
    void *ctx;               // used for passing anything to a callback e.g. pass the lua_State 
    void (*cb)(void *data);  // cb returns nothing and takes a pointer to a struct data
};


int Mockit_gettime(time_t *secs, long *ms);

int Mockit_mstimestamp(uint64_t *timestamp);

int Mockit_bsleep(uint32_t milliseconds, bool do_restart, uint32_t *time_left);

int Mockit_oneoff(uint32_t time, struct data *data);

int Mockit_getit(uint32_t interval, struct data *data);

void Mockit_static_data_init(
                             struct data *dt, 
                             void (*cb)(void *),
                             uint32_t timeout, 
                             void *stuff,
                             bool free_data,
                             bool free_ctx
                             );

struct data *Mockit_dynamic_data_init(
                                      void (*cb)(void *), 
                                      uint32_t timeout, 
                                      void *stuff,
                                      bool free_data,
                                      bool free_ctx
                                      );

int Mockit_destroy(struct data *dt, bool free_data, bool free_ctx);

int Mockit_init(void);

int Mockit_disarm(pthread_t thread_id);


#endif
