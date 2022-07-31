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


Timer expirations and destroying timers
--------------------------------------------

* cover mark of destruction etc


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

