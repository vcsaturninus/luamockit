DESIGN AND IMPLEMENTATION NOTES
====================================

The design isn't the most intuitive one in the context of callbacks but is the
result of a process of having to adapt to what's possible and convenient to do with
Lua.

The problem can be summed up as follows: 
- One can't call back asynchronously into a Lua state; not without the risk of
corrupting the lua-C API stack associated with that state. 

The above has a host of potential solutions but all of them fall somehow short
or prove inconvenient.


Saving lua states/threads
----------------------------

If using coroutines each coroutine has its own lua thread and a C function that is 
called might therefore be called with a lua_State other than the main one.
Instinctively, one would would think to save this state in a C data structure 
and then use it to call back into lua on a timer's expiry, when a registered
callback must be called. However, the state might've well disappeared by then - 
resulting in a crash.
To ensure the lua threads don't go anywhere and avoid the aforementioned issue,
they must be ANCHORED, either via assignment to a lua variable or by saving them
in the lua REGISTRY.

### USING A SINGLE GLOBAL STATE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To do away with this nuisance, an alternative would be to save the state passed
to the first function that opens the library in a global variable to be used by
all the functions in the C library that call back into Lua. However, there's no
guarantee that this state is actually the main state of Lua -- again, if
coroutines are used.

The above two points are made to emphasize the following: 
-- coroutines can greatly complicate matters and are to be avoided.
This library is expected to be called from lua scripts with a single lua state.

-- Unless passed the main lua thread/state (which is the case if there's a SINGLE
lua state/thread i.e. when coroutines aren't used), the state must be ANCHORED to
ensure it doesn't get garbage collected leading to a crash when a call back into it
happens.
This library will anchor the state the Mockit_get_interval_timer() and 
Mockit_register_oneoff_callback() functions get called with, and therefore could in theory
be used even from scripts employing coroutines, but the library can't promise correct 
behavior.

Asynchronously calling back into a Lua State
----------------------------------------------

Even with the above points in mind, the main problem is that one CANNOT call back 
into a lua state asynchronously.

The first problem is, obviously, mutithreading. If mutithreading is being used,
like in this library, then multiple threads simultaneously interacting with the
same lua state will inevitably end badly. 

This problem has two solutions:

1) each POSIX thread can have its own lua State.
This has a whole host of disadvantages: each state is completely separate. The state,
which will be a whole new lua instance, must be opened, populated with the required
libraries etc -- which is slower. It's also very cumbersome to share variables
between two lua states, despite a number of libraries having been written specifically
to address this.


2) Mutexes must be used to serialize the POSIX threads' interaction with the lua state.
This avoids the inevitable race conditions.  

HOWEVER, 2) has another glaring problem: if multiple libraries are used and they all
take the same aproach, then chaos ensues.
Consider the case where another separate library uses the same approach: it saved
the lua state(s), and then uses mutexes to ensure a single POSIX thread calls
back into the state at a time. This library is almost certain to use different
mutexes than luamockit, and therefore even though each library has done away
with race conditions internally, the two libraries themselves are now engaged
in a race as they're bound to interfere with each other.

The Lua interpreter itself has global locks one can use but it's not an option if
one wants or must use the standard lua version provided.


EVENT QUEUE
--------------

The approach this library ultimately settled on is all about giving the power to lua
itself.
Interval and one-off timers generate an 'event' on expiration. The event is simply 
a structure that contains some internal details, primarily the lua callback associated
with this event that must be called back as a result of the timer having expired. 
This event gets added to a global event queue in this library.

*  -- At no point is LUA called back asynchronously.  -- *

The queue simply gets populated, from multiple threads (and therefore enqueing
and dequeing operations must be and are protected by a mutex to ensure serialization).

The lua script itself must periodically call the lua_process_events() function
which will go through the event queue and dequeue each pending event and 'handle' it.
To handle an event means to call the Lua callback associated with it, and then
remove it from the event queue (as explained in the 'timer expirations and destroying timers'
section, there are some minor caveats to this).
 
Therefore each call to lua_process_events() has a backlog that it needs to clear.
The rarer lua_process_events() gets called, the bigger the backlog and the less 
precise the timers. 

That is to say, if the user registers a callback to be called back every 3 seconds
but the lua_process_events gets called every 10 seconds, then the callback will
only get called every 10 seconds and every 10 seconds it will be called 3 times
in a row (because the timer for the callback will have expired 3 times)!

It's therefore important to call the lua_process_events() function as often as 
possible : or, specifically, about as frequently as the callback with the shortest
interval timer.

To solve the above problem and to remove the overhead of constant unnecessary polling,
another function is provided - `lua_wait_for_events()` - 
which makes a blocking call to wait on the list to be populated. It unblocks as
soon as an event is added to the list. If there are events in the queue, the function
returns immediately. With the above in mind, a possible undesirable scenario
is that the function may block forever if no new events get generated. To account for 
this possibility, `lua_wait()` takes an optional parameter, `timeout`. The blocking call
will unblock either as soon as a new event is generated or after timeout milliseconds
-- whichever comes first.


One-off timers and interval timers
-------------------------------------

A one-off timer will sleep for the specified duration in a separate
thread, then exit. An interval timer will _repeatedly_ sleep for the
specified duration in a separate thread _until disarmed_, then exit.

On each timer expiry (**only one for one-off timers, by definition**),
the callback associated with the timer gets called. The callback gets
called in the same thread as the timer i.e. not in the main thread.
It's the responsibility of the user to ensure the callback is
thread-safe and/or reentrant etc as needed.

Timer expirations, timer disarming, and timer destruction
----------------------------------------------------------------

Because interval timers oscillate forever between sleeping and then
waking up and calling the associated callback, there must be a way to
tell them to stop. You do this by _disarming_ them.
Conversely, because one-off timers only sleep and wake up _once_
there is no need to disarm them (and they _cannot_ be disarmed).

Disarming an interval timer may not happen instantaneously. A mark
(`MOCKIT_MFD`, see source code) is set in a certain field of the
struct representing the timer to let the timer know it's been
disarmed. On each wakeup, the timer (I'm using 
_the timer_ as shorthand for _the thread function implementing the
timer functionality_ or _the thread in charge of the timer_) will
check this field and if `MOCKIT_MFD` has been set it knows it's been
disarmed  so it does not call the associated callback again nor does
it go to sleep again: instead, the timer just exits and performs
cleanup actions.

Before doing any cleanup, the timer will set the `MOCKIT_MOD` mark in
the respective struct field, as a way of acknolwedging to the caller
that it's seen the `MOCKIT_MFD` mark i.e. the disarming request.
The user should _not_ check for `MOCKIT_MOD` or any such mark after
the timer terminates as they would be committing `use-after-free`.
However the user could implement a destructor that does not free the
timer resources and can use these (or similar) mark mechanism as a
means of communication. `luamockit` takes this precise approach in
order to synchronize resource release.

### Timer cleanup / destruction

Disarming an interval timer (as mentioned, one-shot timers cannot be
disarmed) simply gets the timer to stop. It does _not_ in and of
itself release the resources associated with the timer back to the
system.

To release said resources, a timer must be _destroyed_. There are a
few nuances here: 
 1. the `pthread` resources associated with the thread itself must be freed
 2. the timer object (`struct mockit`) and any user-allocated memory
    pointed to by the `ctx` field of the `mockit` struct must also be
    freed.

Each will be covered in turn.
 1. Releasing pthread resources

One-off timers always run in `detached` mode, i.e. they do not have to
be `joined`. Nothing special needs to be done here.

Interval timers do _not_, on the other hand, detach themselves. They
must therefore be joined at the end to release their resources back to
the system. Disarming an interval timer and then joining the
associated thread are both done via the common `destroy` function used
with interval timers.

2. Releasing memory associated with the timer object/struct

Both one-off and interval timers are **self-destroying**, meaning they
clean up after themselves when exiting the thread (as explained
below).

At destruction time, a timer will by default simply run `free()` on the 
timer object (`struct mockit`) before exitting the thread. The `ctx`
field is not freed. If the user uses this field (which is reserver for
arbitrary use by the user) to point to some arbitrary memory
(user-allocated or not), they should also provide a destructor
function that knows how to tear down the timer. 

If a destructor function is provided by the user, it should take of
freeing _both_ the `struct mockit` (if dynamically initialized/allocated)
and_ any memory pointed to by the `ctx` field of this struct as 
appropriate. If the user makes use of the `ctx` field and points it 
to some dynamically-allocated chunk of memory but they do not provide 
a suitable destructor, memory leakage may ensue.

It's vital to keep the following points in mind:
 * the user _must_ call `Mockit_destroy()` on _interval_ timers to
   free their resources. `Mockit_destroy()` calls `Mockit_disarm()`
   implicitly on interval timers, hence users need not call the latter
   and should only call the former.

 * one-off timers _cannot_ be destroyed. They're only disarmed and
   then they self-destruct when they get around to it. Calling
   `Mockit_destroy()` on a one-shot timer is completely synonymous
   with caling `Mockit_disarm()`.

 * Note that if the timer is long-run, it may not make sense to wait
   until the timer wakes up before being able to exit the program.
   Imagine the (somewhat absurd) scenario where a timer (interval or
   one-off) is set to a duration/interval of 1h. The user must then
   **wait** for 1h until the timer wakes up and sees `Mockit_MFD` has
   been set, after which it will exit. In such a scenario, it may be
   preferable to simply exit the program without waiting: the
   resources will be released back to the `OS` after program exit
   anyway. This is all the more so with one-off timers as they
   _cannot_ be joined so there's no way to wait for their completion
   other than sleeping.


Lua script design
--------------------

The above points mean that a certain design naturally imposes itself for a lua script
using the interval timers feature of this library (though this is by no means mandatory).
The script will most often take the form of an infinite loop that blocks with 
`lua_wait()` and then calls `lua_process_events()` every time it unblocks.

This makes it most suited to scripts meant to be running as daemons. This might
not be as much of a problem as it sounds, since that's exactly the context where
one would typically even want to have callbacks called asynchronously or at certain 
intervals: event loops dispatching events.

