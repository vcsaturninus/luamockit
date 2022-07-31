How to use (lua)mockit
============================

_Updated Nov 26 2021_


Notes
----------------

 * All the functions in the library take _MILLISECONDS_ as the value
   for their timer argument.


Luamockit : the mockit lua library
-----------------------------------

### Steps: 

 * require the library
```
t = require("luamockit")
```

 * initialize the library:
```
t.init()   -- return true on success, else false
```

 * sleep for n seconds
```
n = 5 * 1000  -- arg is in milliseconds
t.sleep(n)   -- makes a _BLOCKING_ call to nanosleep() to sleep for n ms 
```

 * set up a one-off callback
```
function mycallback()
  print("called callback")
end

t.oneoff(mycallback, 2000)   -- calls mycallback 2 seconds from now;
                             -- doesn't return anything
```

 * set up an interval timer with a callback
```
-- calls mycallback EVERY 2 seconds until stopped (object a is
-- destroyed); getit() returns a userdata that holds an interval timer
-- object; this must be detroyed when you want the callback to no
-- longer be called
a = t.getit(mycallback, 2000) 
```

 * destroy interval timer object
```
a = a:destroy()  -- a must've been returned by a call to t.getit()
             -- returns nil. The return value MUST BE REASSIGNED to
             -- a so that lua can garbage collect the destroyed
             -- object!
```

 * WAIT until an event occurs (see next section 'MOCKIT EVENTS')
```
t.wait() -- returns true when there are events waiting, else false
```

 * process events in event queue
```
t.process_events()  -- returns true on success, else false/fails
```


Luamockit events
------------------------------------

Because it's not safe to call back asynchronously into lua, the design
adopted by luamockit is to represent each timer expiration (which is
associated with a callback that must be called) to an event queue. 
Lua is responsible for checking this event queue and processing events
waiting in it. luamockit NEVER asynchronously calls lua, so the lua
script itself _MUST_ call `luamockit.process_events()` or no events
get processed  -- no callbacks get called.

There are two different approaches you could take: 
 1. poll the event queue by sleeping (calling luamockit.sleep()) for a
    certain period of time, then calling luamockit.process_events().
    These are often called in tandem in an inifinite loop. I.e. sleep,
    process events in queue if any, rinse, repeat. 

 2. make a blocking call to check if there are events in the queue
    using luamockit.wait(). The call unblocks as soon as tehre's an
    event ready and waiting. This is more efficient and the advisable
    approach, since you don't have to constantly be polling, and it's
    also highly more accurate, as you don't risk many events piling up
    in the queue due to a lengthy sleep. 

Only use the polling approach when for one reason or another accuracy
and promptness aren't required or even desirable. E.g. when you _WANT_
to only rarely check the queue, rather than as soon as an event is
added. 

The next two section show sample code for using first the polling
approach and then the more accurate wait() approach.


Luamockit polling approach
----------------------------

This section shows how to register a one-shot callback and then an
interval callback called every 0.2 seconds.

We're assuming `mycallback` is already defined and does some arbitrary
processing and needs to be called at regular intervals.
```
    t=require("luamockit")

    res = t.init()
    if not res then 
      print("failed to initialize library");
      os.execute("exit 1")
    end

    -- register one-shot callback
    t.oneoff(mycallback,2000)
    -- an event for the callback is generated after two seconds, but 
    -- we only process it after 5 seconds!because that's when we
    -- finally
    -- check the queue, after sleeping. Conversely, if we sleep for
    -- only 1 second, for example, we'll never have the callback called 
    -- since the queue is empty at that point; similarly if we never
    -- call process_events() the callback never gets called
    t.sleep(5000)
    t.process_events() -- an event will've have been added for our callback

    cbObj = t.getit(mycallback, 200) -- event gets generated every 0.2 secs
    if not cbObj then print("failed");os.execute("exit"); end
    
    -- for interval callbacks, the program must always sit in a loop,
    -- either an inifinite one, or for the desired number of instances
    while true do
       t.sleep(1000)
       -- every time we wake up and check the queue, 5 events will've
       -- already been generated, since we check every one second but
       -- events get generated every 0.2 secs
       t.process_events()
    end
```


Luamockit wait() approach
----------------------------

This shows the same exact example but it makes use of `t.wait()` rather
than `t.sleep()`.

We're assuming `mycallback` is already defined and does some arbitrary
processing and needs to be called at regular intervals.
```
    t=require("luamockit")

    res = t.init()
    if not res then 
      print("failed to initialize library");
      os.execute("exit 1")
    end

    -- register one-shot callback
    t.oneoff(mycallback,2000)
    t.wait() -- unblocks AS SOON AS the timer expires 
    t.process_events()  -- ==> much more accurate than luamockit.sleep()

    cbObj = t.getit(mycallback, 200) -- event gets generated every 0.2 secs
    if not cbObj then print("failed");os.execute("exit"); end
    
    -- for interval callbacks, the program must always sit in a loop,
    -- either an inifinite one, or for the desired number of instances
    while true do
       t.wait()
       -- wait() unblocks every time the queue is NOT EMPTY. Therefore
       -- we never miss any events due to prolonged or imprecise
       -- sleep, leading to more accurate and pompt results AND no
       -- inefficient polling!
       t.process_events()
    end
```

==========================================================


How to use mockit
======================

This section shows how to use the mockit library directly from a C
program, rather than a lua program. The library is primarily designed
to be used in lua scripts but it can be painlessly used from a C
program just as easily, though the experience is quite a bit
different: specifically, the whole queue adding and queue checking
design is out the window as there's no need for it. That was simply 
put in place to deal with the fact that we can't safely make
asynchronous calls into Lua.

To use the `mockit` functionality in C, you would simply just use
`mockit.c` and **completely skip/ignore** `luamockit.c`.

Below is a demonstration of all the functions you could use in C:
```
    // cat t.c
    // compile this file with `gcc t.c mockit.c -lpthread -lrt -o q`
    #include <stdio.h>

    #include "mockit.h"

    // function prototype MUST be of a function
    // that takes a void ptr and returns a void ptr
    void *one_off_callback(void *arg){
        struct data *dt = arg;
        printf("%s\n", (char *)dt->stuff);
        return NULL;
    }

    void *interval_callback(void *arg){
        struct data *dt = arg;
        printf("%s\n", (char *)dt->stuff);
        return NULL;
    }
    //////////////////////////////////////////////////////////////



    int main(int argc, char **argv){
        // sleeping
        ssize_t left = 0;
        int error = 0;
        Mockit_bsleep(1000, &left, &error);
        printf("done sleeping. Time left: %zu, error: %i\n", left, error);

        // ********************************

        // one-off callback timer registration
        struct data *data = Mockit_data_init();
        data->cb = one_off_callback;
        char msg[] = "called one-shot callback";
        data->stuff = msg;
        data->timer_id = 0;
        Mockit_oneoff(3000, data);
        // the one-off callback  gets called asynchronously so we must either
        // sleep or sit in a loop so that the program doesn't exit before the timer
        // for the callback expires
        sleep(4);
        int res = Mockit_destroy(data);
        printf("res is %i\n", res);
        
        // ********************************

        // interval timer callback
        struct data data2;
        data2.cb = interval_callback;
        char msg2[] = "called interval callback";
        data2.stuff = msg2;
        Mockit_getit(3000, &data2);
        // we sleep 10 seconds which means there's enough time for the interval
        // callback to be called 3 times
        sleep(10);

        // NOTE:
        // you ONLY call Mockit_destroy() on the data struct if it was dynamically allocated
        // eg. through Mockit_data_init()
        // Here it was statically allocated so destroy() must NOT be called.
}
```

Mockit, pthreads, and memory leaks
---------------------------------------

If you run valgrind on the C program shown above, you'll see this:
(NOTE: a lua program will show more memory leaks because there are a
lot of opaque things that valgrind won't be able to figure out. For
example, lua handles memory allocated for userdata, has destructors
etc)
```
[...]
    ==434248==
    ==434248== HEAP SUMMARY:
    ==434248==     in use at exit: 544 bytes in 2 blocks
    ==434248==   total heap usage: 44 allocs, 42 frees, 4,222 bytes allocated
    ==434248==
    ==434248== Searching for pointers to 2 not-freed blocks
    ==434248== Checked 8,507,384 bytes
    ==434248==
    ==434248== 272 bytes in 1 blocks are possibly lost in loss record 1 of 2
    ==434248==    at 0x483DD99: calloc (in /usr/lib/x86_64-linux-gnu/valgrind/vgpreload_memcheck-amd64-linux.so)
    ==434248==    by 0x40149CA: allocate_dtv (dl-tls.c:286)
    ==434248==    by 0x40149CA: _dl_allocate_tls (dl-tls.c:532)
    ==434248==    by 0x4871322: allocate_stack (allocatestack.c:622)
    ==434248==    by 0x4871322: pthread_create@@GLIBC_2.2.5 (pthread_create.c:660)
    ==434248==    by 0x109902: Mockit_oneoff (in /home/common/playground/repos/mockit/src/mytest)
    ==434248==    by 0x109512: main (in /home/common/playground/repos/mockit/src/mytest)
    ==434248==
    ==434248== 272 bytes in 1 blocks are possibly lost in loss record 2 of 2
    ==434248==    at 0x483DD99: calloc (in /usr/lib/x86_64-linux-gnu/valgrind/vgpreload_memcheck-amd64-linux.so)
    ==434248==    by 0x40149CA: allocate_dtv (dl-tls.c:286)
    ==434248==    by 0x40149CA: _dl_allocate_tls (dl-tls.c:532)
    ==434248==    by 0x4871322: allocate_stack (allocatestack.c:622)
    ==434248==    by 0x4871322: pthread_create@@GLIBC_2.2.5 (pthread_create.c:660)
    ==434248==    by 0x4890BCC: __start_helper_thread (timer_routines.c:176)
    ==434248==    by 0x487947E: __pthread_once_slow (pthread_once.c:116)
    ==434248==    by 0x488F9A2: timer_create@@GLIBC_2.3.3 (timer_create.c:101)
    ==434248==    by 0x109AE1: Mockit_getit (in /home/common/playground/repos/mockit/src/mytest)
    ==434248==    by 0x109598: main (in /home/common/playground/repos/mockit/src/mytest)
    ==434248==
    ==434248== LEAK SUMMARY:
    ==434248==    definitely lost: 0 bytes in 0 blocks
    ==434248==    indirectly lost: 0 bytes in 0 blocks
    ==434248==      possibly lost: 544 bytes in 2 blocks
    ==434248==    still reachable: 0 bytes in 0 blocks
    ==434248==         suppressed: 0 bytes in 0 blocks
    ==434248==
    ==434248== ERROR SUMMARY: 2 errors from 2 contexts (suppressed: 0 from 0)
```

There are various things to note: 
 * the thread we started for the oneoff timer is counted as a memory
   leak; EDIT: actually, DETACHING this thread with
   `pthread_detach(pthread_self())` gets rid of  this!

 * the same goes for the interval callback timer set up -- one thing
   to note here is that the timer expiry mechanism where our thread
   function gets called in a new thread involved a single thread being 
   created, it seems. I.e. the thread function always seems to get
   called in the same thread away from the main thread -- rather than
   each timer expirity creating a _whole new_ thread (else we have in
   this program three timer expiries so we would've had three, not
   one, threads counted as leaks for the timer interval expiries)

 * even though we make the (one-off functions') thread detached and
   even call pthread_exit() it still gets counted as a leak 


The leaks aren't really 'leaks': the memory does get released when
main() returns, but valgrind misses this and so at the point where
valgrind stops tracking memory, the memory used by pthreads would
indeed look like leakage.
To avoid this you would have to ensure that the main thread exits
_AFTER_ the other threads. This can be done either by calling
`pthread_exit()` or `pthread_join()` in main(). For `join`, the
threads must be _Joinable_. You COULD call pthread_exit() in main
before returning: this will exit from the main thread but will not
return: it will only return when all the other threads have exitted. 
The issue with this is that I've tried it and I think the interval
mockit timer does NOT exit and so the program ends up hanging
indefinitely without actually exitting. 

Crucially,  the 'memory leaks' seem to have constant complexity with
respect to the number of calls made to a callback, but linear with
respect to the number of timers set up: I.e. if you set up two oneoff
timers and three interval timers, you'll have 5 `blocks` in valgrind's output 
(5 threads, really, here) counted as leaks. But if you make 10000
calls (to the callback associated with the interval timer) from one of the
interval timers, that has no impact on the respective thread
accounting for more of a leak that the other threads or compared to if
it had made more or fewer calls to the associated callback.

EDIT:
Actually, the leak above is not per thread: it's a one-off cost you
pay to set up timers. I created 5 different timers, destroying all of
them (note: you MUST destroy them, because dynamic memory is indeed 
allocated under the hood and associated with timers) and at the end
the same number of leaks and the same amount of memory (as seen in the
output above -- copied here as well) ends up being there.
```
    ==114789== 272 bytes in 1 blocks are possibly lost in loss record 1 of 1
    ==114789==    at 0x483DD99: calloc (in /usr/lib/x86_64-linux-gnu/valgrind/vgpreload_memcheck-amd64-linux.so)
    ==114789==    by 0x40149CA: allocate_dtv (dl-tls.c:286)
    ==114789==    by 0x40149CA: _dl_allocate_tls (dl-tls.c:532)
    ==114789==    by 0x487C322: allocate_stack (allocatestack.c:622)
    ==114789==    by 0x487C322: pthread_create@@GLIBC_2.2.5 (pthread_create.c:660)
    ==114789==    by 0x486DBCC: __start_helper_thread (timer_routines.c:176)
    ==114789==    by 0x488447E: __pthread_once_slow (pthread_once.c:116)
    ==114789==    by 0x486C9A2: timer_create@@GLIBC_2.3.3 (timer_create.c:101)
    ==114789==    by 0x109ABE: Mockit_getit (mockit.c:223)
    ==114789==    by 0x10950F: main (t.c:52)
    ==114789== 
```
This means this is not an actual memory leak, but a constant that
never increases. As such, it can be safely disregarded.
