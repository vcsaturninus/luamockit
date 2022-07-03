#ifndef MO_CLOCK_INTERVAL_TIMER
#define MO_CLOCK_INTERVAL_TIMER

#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>

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
    timer_t    timer_id__ ;

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
                             void *stuff
                             );

struct data *Mockit_dynamic_data_init(
                                      void (*cb)(void *), 
                                      uint32_t timeout, 
                                      void *stuff
                                      );

int Mockit_destroy(struct data *dt, bool free_data, bool free_ctx);

#endif
