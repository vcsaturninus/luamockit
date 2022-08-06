Luamockit Design and Implementation Notes
===========================================

##### Table of Contents
 * [Introduction](#introduction)
 * [Saving Lua States](#saving-lua-states)
 * [Using a Single Global Lua State](#using-a-single-global-lua-state)
 * [Asynchronously Calling Back into a Lua State](#asynchronously-calling-back-into-a-lua-state)
 * [Event Queue Design](#event-queue-design)
    * [One-off and Interval Timer Termination](#one-off-and-interval-timer-termination)
    * [Enqueued Events and Pending Events](#enqueued-events-and-pending-events)
    * [Event Mark and Timer Mark](#event-mark-and-timer-mark)

Introduction
------------------

The design of `luamockit` isn't the most intuitive one in the
context of callbacks but is the result of a process of having to
adapt to what's possible, convenient, and safe to do with Lua.

The problem can be summed up as follows:
- One can't call back asynchronously into a Lua state; not
  without the risk of corrupting the lua-C API stack
  associated with that state.

The above has a host of potential solutions but all of them fall
somehow short or prove inconvenient.

Saving lua states
----------------------------

If using coroutines, each coroutine has its own Lua state/thread and
a C function that is called might therefore be called with a
`lua_State` other than the main one.
Instinctively, one would would think to save this state in a C
data structure and then use it to call back into lua on a timer's
expiry, when a registered callback must be called. However,
the state might've well disappeared by then - resulting in a crash.
To ensure the lua threads don't go anywhere and avoid the
aforementioned issue, they must be _ANCHORED_, either via assignment
to a lua variable or by saving them in the Lua REGISTRY.

Using a single global Lua state
--------------------------------

To do away with this nuisance, an alternative would be to save
the state passed to the first function that opens the library
in a global variable to be used by all the functions in the C
library that call back into Lua. However, there's no guarantee that
this state is actually the main state of Lua -- again, if
coroutines are used.

The points made above are to emphasize the following:
 - coroutines can greatly complicate matters and are to be avoided.
   This library is expected to be called from lua scripts with
   a single lua state.

 - Unless passed the main lua thread/state (which is the case if
   there's a _SINGLE_ Lua state/thread i.e. when coroutines aren't used),
   _the state must be ANCHORED_ to ensure it doesn't get garbage collected
   leading to a crash when a call back into it happens.

This library will anchor the state the `Mockit_get_interval_timer()` and
`Mockit_register_oneoff_callback()` functions get called with, and
therefore could in theory be used even from scripts employing coroutines,
but the library can't promise correct behavior.

Asynchronously calling back into a Lua State
----------------------------------------------

Even with the above points in mind, the main problem is that one CANNOT
call back into a lua state asynchronously.

The first problem is, obviously, mutithreading. If mutithreading is
being used, like in this library, then multiple threads simultaneously
interacting with the same lua state will inevitably end badly.

This problem has two solutions:

1) each POSIX thread can have its own lua State.
This has a whole host of disadvantages: each state is completely
separate. The state, which will be a whole new lua instance,
must be opened, populated with the required libraries etc -- which
is slower. It's also cumbersome to share variables between two Lua
states, despite a number of libraries having been written specifically
to address this.

2) Mutexes must be used to serialize the POSIX threads' interaction
with the lua state. This avoids the inevitable race conditions.

HOWEVER, 2) has another glaring problem: if multiple libraries are
used and they all take the same aproach, then chaos ensues.
Consider the case where another separate library uses the same approach:
it has saved the lua state(s), and then uses mutexes to ensure a
single POSIX thread calls back into the state at a time. This library
is almost certain to use different mutexes than `luamockit`, and therefore
even though each library has done away with race conditions internally,
the two libraries themselves are now engaged in a race as they're bound
to interfere with each other.

The Lua interpreter itself has global locks one can use but it's
not an option if one wants or must use the standard lua version provided
and cannot recompile it and modify it.

Event Queue Design
---------------------

The approach ultimately adopted by `luamockit` is to have Lua take the
initiative. No asynchronous call back into any Lua state is ever made.
Instead, lua callbacks are associated with a timer (one-off or
cyclic). This timer calls a callback on expiry. In C, this is the
actual callback the user registers. For Lua, however, this is a C
function that simply creates a timer expiry 'event' and puts it
in a queue. This event contains internal data such as the Lua
callback to call and the state to call back into.

The timers keep populating this queue as producers (until disarmed,
see the main `README.md`) and the events remain there until the user
drains the queue _from Lua_ using `process_events()`. Then each event
in the queue is `handled`: the lua callback gets called (i.e. here is
when the call back into lua happens) and the event struct gets
destroyed. In other words, the _Lua_ callbacks get called _not_ when
the associated timer expires, but when the user decides to process the
events in the queue.

This solves the aynchronous call back into Lua problem but the
mechanics of properly destroying a one-off or interval timer
such that associated resources are released and correct and
thorough cleanup is effected - is still complicated. The solution
to that is described next.

### One-off and Interval Timer Termination

One-off timers can neither be destroyed nor disarmed from within Lua
so their case is much the simpler of the two. Once a one-off timer
has expired it will call a C callback that creates an event that gets
placed in the queue. When the `process_events()` function is called it
will iterate over the queue and handle each event in turn; when it comes
across a one-off timer event it knows no others will follow for that
timer and that the thread will have already exited, so after handling
the event it proceeds to also remove any lingering resources
associated with the one-off timer. Once this is done, the timer is
considered destroyed.

Interval timer termination is more complex because the
`process_events()` function needs a way of knowing whether it's
looking at the last event for a specific timer. The mechanism used
for this purpose is twofold:
 1. the mark facility of each timer is used to mark the next-to-last
    (disarmed) and the very last (ready for destruction) events for a
    specific timer
 2. resource release is _delayed_ such that `process_events` is the
    one calling `free` (if necessary), and _not_ the exiting timer
    thread or any destructor registered to be called by it.


The timer is marked with `MOD` from within the exiting thread to
acknowledge the fact that this timer has  been disarmed and
now it's going to be destroyed.

Normally no one ever sees (and can therefore have no use for) the
`MOD` mark since the timer will be destroyed immediately after.
However, for the purposes of `luamockit` a custom destructor is
supplied that prevents the deallocation of the timer struct. The thread
_will_ exit and clean up after itself but the destructor will generate
a new - and last - event for this timer, putting it in the queue as
well.

When an interval timer is _disarmed_ (which the `destroy` metamethod
calls implicitly), `MFD` is set for the timer. The timer oscillates
between sleeping and calling the callback. On each wakeup it checks
for `MFD` being set. If it is, it breaks out of its loop, sets `MOD`,
then either performs default cleanup or calls a registerd the
destructor before exiting the thread.

The above means an event is _always_ generated for a timer that has
`MFD` set. `process_events()` knows when it sees such an event that a
a last one with `MOD` set will follow, and it can perform cleanup
then. It _cannot_ perform cleanup _now_ because if it were to call
`free()` on the timer struct the timer or the registered destructor,
if they run, would be performing 'use after free'.
Normally the thread simply exits after setting `MOD` though, without
calling the registed callback again, so no new event can be generated.
`Luamockit` instead used a custom destructor that puts another event
in the queue such that `process_events()` can see the expected last
event with `MOD` set. The destructor does simply that and does _not_
deallocate anything. Instead, deallocation is delayed and left to
`process_events()` to perform. If the destructor were to call `free()`
on the timer struct then `process_events()` would be committing 'use
after free'!

With the above mechanism, `process_events()`, in sum, operates as
follows:
 * one-off timer events are handled, and their resources immediately
   released at the same time
 * interval timer events that have _neither_ `MOD` nor `MFD` set are
   handled.
 * interval timer events that have `MFD` set are _not_ handled
   (callback is not called). An `MOD`-marked event is expected to
   follow.
 * interval timer events that have `MOD` set are _not_ handled
   (callback is not called). This is the last event that will be
   generated for this specific timer so all the associated
   resources can be safely `free`d.

**NOTE** that `process_events()` will only see `MFD` and `MOD`-marked
events for a specific timer if that timer is _disarmed_ but not
_destroyed_ by the Lua user. This may be when the user has specified
the optional `timeout` parameter to the `destroy` function and
`destroy()` ended up timing out before the timer had a chance to wake
up. This should be rather uncommon because the user is encouraged to
not specify a timeout so cleanup is correctly performed. Therefore
under normal circumstances `process_events()` should only
see one-off timers and non-marked interval timers.

If interval timers are called `destroy()` on as they normally should
without the optional _timeout_ parameter, then the `destroy()` function
is the one that will actively monitor for `MFD` and `MOD`-marked
events associated with the timer in question.

Specifically, when the user calls `destroy()` on an interval timer:
 * `disarm()` is called, which sets `MFD`.
 * when the timer wakes up, it sets `MOD`, then calls the custom
   destructor, then exits its thread.
 * the custom destructor places another event (`MOD`-marked) in the
   queue
 * the `destroy()` function will look for `MFD` or `MOD`-marked events
   associated with the timer in question. It skips the `MFD`-marked
   event, and it releases all resources associated with the timer
   when it sees the `MOD`-marked event.
 * the `destroy()` function then `joins` the exited thread so that its
   thread resources can be released back to the system
 * the timer is now considered destroyed and the function returns
   back to Lua.

Note that by 'skip' I mean the event is unhandled. It's still
_removed_ from the queue though. It's just that the Lua callback does
not get called for that event.

### Enqueued Events and Pending Events

Interval timers that are marked with either `MFD` or `MOD` are not
handled, i.e. the associated `Lua` callback is **not** called. It
makes sense for this to be so because both of these follow the
instant when the user calls `destroy()` on the given interval timer.
Any subsequent events for the timer should therefore be discarded.
Only two events follow after the point when the user calls
`destroy()`: one carrying the `MFD` mark, one carrying the `MOD` mark.
These are consequently both not handled but are necessary for
`luamockit` to perform correct cleanup of resources.

There are therefore two types of events in the event queue at any
given time: events that to be handled, and events that will not be
handled.

`luamockit` exposes two functions to get these numbers:
 * `pending()` will return the number of events currently in the queue
   that are to be handled (i.e. interval timers with both `MFD` and
   `MOD` unset, and one-off timers)
 * `inqueue()` will return the count of _all_ the events currently in
    the queue.


### Event Mark and Timer Mark

Each `struct mockit` timer struct has a `mark` field where `MFD` and
`MOD` are set. Above I repeatedly referred to 'MFD and MOD-marked
events'. This needs some clarification.

Each event struct associated with the same timer has a pointer to that
timer struct. If there are 10 events in the queue at a given time,
then one can see how changing the timer struct's mark can be a
problem: all events would have the mark changed. Needless to say,
setting `MOD` at any time in the timer struct would then set it
for the first event struct in the queue as well, which would completely
break the cleanup/destruction protocol since it no longer holds true
that once luamockit has seen an `MFD`-marked event for a specific
timer one last `MOD`-marked event for that timer will follow.
Luamockit will therefore try to free resources for _all_ `MOD`-marked
events associated with that timer, thinking it's the last, and
crashing the system.

Instead, it's necessary for each event struct to have its own mark
independent of other events. This mark is a **copy** of the timer
struct's mark made at the time of event generation and enqueueing.
Since each thread is in charge of a single timer, it's always the same
thread generating events for a timer and enqueueing them and therefore
there is no race condition when copying the mark.

