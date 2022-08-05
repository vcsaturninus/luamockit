Mockit - C and Lua timer callback library
==================================================

##### Table of Contents  
* [Mockit](#Mockit)  
    * [Mockit_gettime](# Mockit_gettime)  
    * [Mockit_mstimestamp](# Mockit_mstimestamp)  
    * [Mockit_bsleep](# Mockit_bsleep)  
    * [one-off timers](# one-off/one-shot timers)  
    * [interval timers](# Interval/cyclic timers)  
* [Luamockit](# Luamockit)


Mockit
--------

### `Mockit_gettime()`: get (seconds, milliseconds) time tuple

get seconds since the Unix epoch and milliseconds since the last second.

```C
time_t secs = 0;
long msecs =  0;
Mockit_gettime(&secs, &msecs);
printf("%zus, %lums\n", secs, msecs); // e.g. 1659641952s, 561ms
```

### `Mockit_mstimestamp()` : get milisecond timeLuamockit
Timestamps in seconds since the unix epoch are extremely common. The
`date` utility can be used for this in the shell, the `C` standard
library's `time()` returns such a timestamp, and so forth.
It's sometimes the case that _millisecond_ timestamps are needed
though. One normally has to use more precise APIs for this purpose. 
`Mockit_mstimestamps()` aims to be such a convenience function.
```C
uint64_t msts = 0;
Mockit_mstimestamp(&msts);
printf("msts: %" PRIu64 "\n", msts);  // e.g. msts: 1659642227734
```

### `Mockit_bsleep()`: go to sleep for specified milliseconds

The function `sleep()` is often too coarse; the function `nanosleep()`
may be too heavy and inconvenient to use. `Mockit_bsleep()` is aimed
to be a more approachable API that takes a millisecond specificiation.
```C
/* sleep for 0.3 seconds, and resume on interrupts */
Mockit_bsleep(300, true, NULL);

/* sleep for 2.1 seconds and do not resume if interrupted by signal
handlers */
Mockit_bsleep(2100, false, NULL);

/* sleep for 0.07 seconds ie 70 ms */
Mockit_bsleep(70, false, NULL);
```

### One-off/one-shot timers

A one-off aka one-shot timer is a sleep in a separate thread for the
specified duration which ends with a user-registered callback being
called on timer expiry. Since by definition this 'timer' only expires
_once_, it is only _once_ that the callback gets called.

![Sequence diagram of one-off timers](docs/mockit_one-off_timer.svg "One-off timers")

The callback is expected to have a certain prototype and the timer
itself requires certain cleanup on teardown. The user may (or may
_need_ to) also register a destructor callback that knows how to clean
things up if using the 'ctx' extension - which allows the user to pass
additional arbitrary data to the timer callback.
```C
void my_callback(void *dt){
    printf("called back!\n");
}

int main(int argc, char **argv){
    struct mockit mit = {0};
    Mockit_static_init(&mit,
                      my_callback,
                      1200,
                      false,
                      NULL,
                      NULL);

    Mockit_arm(&mit);
    Mockit_bsleep(3500, true, NULL);
}
```

Note **one-off** timers do _not_ need to be explicitly destroyed via
`Mockit_destroy()` the way interval timers (covered next) do. A
destructor may be registed if teardown is somehow special, otherwise
one-off timers can clean up after themselves.

Below is another example with a timer augmented with user-allocated
data which a custom destructor is registered for that knows how to
tear it down.
```C
void my_callback(void *dt){
    struct mockit *mit = dt;
    int *i = mit->ctx;
    printf("called back (%i) !\n", *i);
}

int my_destructor(void *dt){
    struct mockit *mit = dt;
    free(mit->ctx);
    free(mit);

    return 0;
}

int main(int argc, char **argv){
    int *myint = calloc(1, sizeof(int));
    assert(myint);
    *myint = 187;

    struct mockit *mit = NULL;
    mit = Mockit_dynamic_init(my_callback,
                        1200,
                        false,
                        (void *)myint,
                        my_destructor);

    Mockit_arm(mit);
    Mockit_bsleep(3500, true, NULL);
}
```

### Interval/cyclic timers

While one-off timers are inherently self-disarming, interval aka
cyclic timers will keep going until explicitly disarmed.
They'll specifically repeat this sequence of actions (in a separate
thread):
 * sleep for specified interval duration
 * if `MFD` mark is not set (set via the
   `Mockit_disarm`/`Mockit_destroy` functions), then
   call the registered callback and go back to sleep.

_Iff_ `MFD` _is_ set, the timer will break out of this loop, set `MOD`
in acknowledgement, _not_ call the callback, and then perform either
default cleanup or otherwise call a destructor function, if
registered.

![Sequence diagram of interval timers](docs/mockit_interval_timer.svg "Interval timers")

An example of an interval timer with default cleanup is shown next:
```C
void my_callback(void *dt){
    struct mockit *mit = dt;
    printf("called back \n");
}

int main(int argc, char **argv){
    struct mockit *mit = NULL;
    mit = Mockit_dynamic_init(my_callback,
                        500,
                        true,
                        NULL,
                        NULL);

    Mockit_arm(mit);
    Mockit_bsleep(1300, true, NULL);
    Mockit_destroy(mit);
}
```


A second example with user-allocated data and a destructor is shown:
```C
void my_callback(void *dt){
    struct mockit *mit = dt;
    int *i = mit->ctx;
    printf("called back (%i) !\n", *i);
}

int my_destructor(void *dt){
    struct mockit *mit = dt;
    free(mit->ctx);
    free(mit);

    return 0;
}

int main(int argc, char **argv){
    int *myint = calloc(1, sizeof(int));
    assert(myint);
    *myint = 187;

    struct mockit *mit = NULL;
    mit = Mockit_dynamic_init(my_callback,
                        1230,
                        true,
                        (void *)myint,
                        my_destructor);

    Mockit_arm(mit);
    Mockit_bsleep(3500, true, NULL);
    Mockit_destroy(mit);
}
```

=========================================================

Luamockit
-----------

![Luamockit system design diagram](docs/luamockit.svg "Luamockit
design diagram")



