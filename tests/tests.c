#include <stdio.h>
#include <stdint.h>   // uint64_t etc
#include <time.h>     // clock_gettime, clock_nanosleep
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>   // getenv
#include <pthread.h>
#include <unistd.h>

#include "mockit.h"

static int tests_run =    0;
static int tests_passed = 0;

// one-off and interval callback should update this
static uint64_t post = 0;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;


// defined in mockit.c
extern void timespec_add_ms__(struct timespec *timespec, uint32_t ms);

// the millisecond error margin
//#define ERROR_RANGE 1
#define ERROR_RANGE 3

// test runner
#define run_test(f, ...) \
    tests_run++; \
    passed = f(__VA_ARGS__); \
    printf(" * test %i %s\n", tests_run, passed ? "PASSED" : "FAILED !!!"); \
    if (passed) tests_passed++;

// conditional printf
#define reveal(fmt, ...) if (getenv("DEBUG_MODE")) fprintf(stderr, fmt, ##__VA_ARGS__);

int data_destructor(void *mit){
    struct mockit *m = mit;
    free(m->ctx);
    free(m);
    puts("in destructor, freed data");

    return 0;
}


/* Ascertain that Mockit's mstimestamp feature works 
   correctly. Return true on success, false on failure.
*/
bool test_mstimestamp(uint64_t ms){
    struct timespec spec;
    assert(!clock_gettime(CLOCK_REALTIME, &spec));
    
    // get a ms timestamp to start from
    uint64_t expected = spec.tv_sec * 1000 + (spec.tv_nsec / 1000000);
    uint64_t secs = ms / 1000;
    uint64_t msecs = ms % 1000;
    expected += secs*1000;
    expected += msecs;

    // sleep for ms duration; after that get a new timestamp and compare
    // what's expected with what we get (from Mockit_mstimestamp) and with
    // what is ACTUALLY the case. All three should be the same.
    timespec_add_ms__(&spec, ms);
    assert(clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &spec, NULL) == 0);
    uint64_t reported = 0;
    assert(!Mockit_mstimestamp(&reported));
    
    struct timespec actual; 
    assert(!clock_gettime(CLOCK_REALTIME, &actual));

    reveal("expected is %lu and reported is %lu and actual is %lu%lu\n", expected, reported, actual.tv_sec, actual.tv_nsec / 1000000);

    if (! (expected <= reported+ERROR_RANGE && expected >= reported-ERROR_RANGE)){
        return false;
    }
    return true;
}

/*
   Verify that the millisecond-precision Mockit_bsleep() is accurate.
*/
bool test_bsleep(uint64_t ms){
    uint64_t start = 0;
    assert(!Mockit_mstimestamp(&start)); 
    
    uint64_t expected = start+ms;
    assert(!Mockit_bsleep(ms, true, NULL));
    uint64_t actual = 0;
    assert(!Mockit_mstimestamp(&actual)); 

    // should have slept for actual - start ms in total
    reveal("expected=%lu, actual=%lu; was expected to sleep for %lu ms\n", expected, actual, actual-start);

    if (! (expected <= actual+ERROR_RANGE && expected >= actual-ERROR_RANGE)){
        return false;
    }
    return true;
}

/* 
   callback function for tests setting up one-off timers.

   NOTE -- No timers currently run in parallel so there's 
   really no need for the mutexes. However, they should be 
   kept in case such tests are added in the future as the 
   mutex locking/unlocking is negligible for our purposes here.
*/
static void OneOffCallback(void *stuff){
    reveal("%s\n", "CALLED: inside one-off callback");
    assert(!pthread_mutex_lock(&mtx));
    assert(!Mockit_mstimestamp(&post)); 
    assert(!pthread_mutex_unlock(&mtx));
}

/* 
   callback function for tests setting up interval timers.

   See the comments under OneOffCallback about the use of mutexes.
*/
static void IntervalCallback(void *stuff){
    reveal("%s\n", "CALLED: inside interval callback");
    assert(!pthread_mutex_lock(&mtx));
    ++post;
    assert(!pthread_mutex_unlock(&mtx));
}

/*
   Verify that the Mockit feature for one-off
   timers is precise and accurate.

   The test registers the one-off callback to be 
   invoked in MS milliseconds from the moment 
   in which this function is invoked. The callback,
   when called will assign the variable 'post' to
   a millisecond timestamp obtained at the moment
   it was called.
   This timestamp is expected to be equal to
   NOW+ms (where NOW is the moment in which 
   test_oneoff_timer() is invoked) otherwise the test
   fails.
*/
static bool test_oneoff_timer(uint64_t ms){
    struct mockit mit;
    Mockit_static_init(&mit, OneOffCallback, ms, false, NULL, NULL);
    
    uint64_t start = 0;
    assert(!Mockit_mstimestamp(&start));
    uint64_t expected = start+ms;

    assert(!Mockit_arm(&mit));
    // sleep 1 second past the point where the timer should've expired,
    // to err on the side of caution
    assert(!Mockit_bsleep(ms+1000, true, NULL));
    reveal("expected=%lu, one-off timer posted=%lu\n", expected, post);

    if (!(expected <= post+ERROR_RANGE && expected >= post-ERROR_RANGE)){
        post = 0;
        return false;
    }
    post = 0;
    return true;
}

/*
   Verify that the Mockit feature for interval timers
   is precise and accurate.
    
   The test involves registering the function IntervalCallback
   with an cyclic timer to run at intervals of INTERVAL milliseconds.
   As soon as DURATION is reached (in milliseconds), the timer gets 
   disarmed.
   Every time the callback gets called, it increments the variable 'post'
   by one. If the callback has run 10 times, then `post` will be 10.
   Otherwise the test has failed.

   It can be easily determined how many times the timer SHOULD have called
   IntervalCallback by the formula `duration / interval`. However, depending 
   on kernel scheduling, the cpu etc it's very likely some of these tests would 
   be off by one, particularly if the interval is very small (e.g. 1ms) and/or
   the durataion is a multiple of the interval: e.g. a duration of 30 ms
   with an interval of 10ms: if the interval timer itself is delayed by 1 ms
   and duration (which is used for sleeping) elapses before it,
   the callback would only be called twice.
   Therefore we should allow for an error marging of 1.
*/
static bool test_interval_timers(uint64_t duration, uint64_t interval){
    struct mockit *mit = Mockit_dynamic_init(IntervalCallback, interval, true, NULL, data_destructor);
    
    uint64_t start = 0;
    assert(!Mockit_mstimestamp(&start));
    uint64_t expected_total_calls = duration / interval;

    assert(!Mockit_arm(mit));
    assert(!Mockit_bsleep(duration, true, NULL));
    assert(!Mockit_destroy(mit));
    
    reveal("Interval callback was called %lu times. Expected: %lu+/-%i\n", post, expected_total_calls,ERROR_RANGE);

    if (!(expected_total_calls <= post+ERROR_RANGE && expected_total_calls >= post-ERROR_RANGE)){
        post = 0;
        return false;
    }
    post = 0;
    return true;
}


int main(int argc, char **argv){
    bool passed = false;

    puts("==================== Running Mockit C tests ======================== ");
    puts("* * * (Any failure invalidates subsequent results) * * *");

    fprintf(stderr, " @ Ascertaining accuracy and precision of Mockit_mstimestamp()\n");
    run_test(test_mstimestamp, 2000);
    run_test(test_mstimestamp, 1313);
    run_test(test_mstimestamp, 17);
    run_test(test_mstimestamp, 8);
    run_test(test_mstimestamp, 1);
    run_test(test_mstimestamp, 3771);
    run_test(test_mstimestamp, 222);
    run_test(test_mstimestamp, 66);

    fprintf(stderr, " @ Ascertaining accuracy and precision of Mockit_bsleep()\n");
    run_test(test_bsleep, 9);
    run_test(test_bsleep, 99);
    run_test(test_bsleep, 999);
    run_test(test_bsleep, 9999);
    run_test(test_bsleep, 1101);
    run_test(test_bsleep, 203);
    run_test(test_bsleep, 30);
    run_test(test_bsleep, 1);

    fprintf(stderr, " @ Running precision tests for one-off timers ...\n");
    run_test(test_oneoff_timer, 8123);
    run_test(test_oneoff_timer, 3);
    run_test(test_oneoff_timer, 13);
    run_test(test_oneoff_timer, 1);
    run_test(test_oneoff_timer, 777);
    run_test(test_oneoff_timer, 3921);
    run_test(test_oneoff_timer, 935);
    run_test(test_oneoff_timer, 19);
    run_test(test_oneoff_timer, 91);

    fprintf(stderr, " @ Running precision tests for interval timers ...\n");
    run_test(test_interval_timers, 2000, 11);  // fails
    run_test(test_interval_timers, 30, 1);     // fails
    run_test(test_interval_timers, 721, 50);
    run_test(test_interval_timers, 3780, 100);
    run_test(test_interval_timers, 5000, 1000);
    run_test(test_interval_timers, 400, 150);
    run_test(test_interval_timers, 8000, 900);
    run_test(test_interval_timers, 103, 10);

    fprintf(stderr, "passed: %i of %i\n", tests_passed, tests_run);
    if (tests_passed != tests_run) exit(EXIT_FAILURE);
    exit(EXIT_SUCCESS);
}
