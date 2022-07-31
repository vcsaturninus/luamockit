Mockit - Lua and C timer callback library
===========================================

## Overview

`mockit` is something of an (unsuccessful) blend of various words
(`clock`, `interval timer`) that sum up the main features that this
library provides or makes use of.

`mockit` is split into two components - a C library, usable from C
code independently, and a Lua library wrapper, that exposes the C
library to Lua along with some additional features. 

While there are some functions that would be convenient to use from C
it's clear the primary target is Lua: C already has features for
sleeping, getting the time in millisecond precision (well, _nanosecond_)
and setting interval timers to the same accuracy -- while Lua is
missing these.

The primary feature this library seeks to make available to both C and
Lua is a system for one-off and interval callbacks: that is to say, the 
ability to have a function be called back once after a specified period, 
or repeatedly at a specified interval.

Additionally, a few more functions are made available for
convenience's sake that allow you to:
 * sleep with millisecond precision in a way that avoids oversleeping 
   and automatically resumes sleeping when interrupted by a signal
   (handler).
 * get the current time as a tuple of seconds since the epoch and milliseconds
   since the last second.
 * get a millisecond timestamp (useful because the usual Unix timestamp is typically
   provided in seconds only).

As discussed at length below, due to the nature of the Lua-C
interaction, a certain design is more or less imposed on both this library and
Lua scripts using it.

------------------------------------------------------------

## Luamockit General Design Principles

  * For a more extensive discussion on the motivations that ultimately
    led to this design, see the comments at the top of `src/luamockit.c`.
    Note that there the functions are referred to by the `C`
    names they have in that file (e.g. `lua_process_events()`) while in
    this `README.md` section they're referred to by the names they
    would be used with in a Lua scriipt (e.g. `luamockit.wait()`).

Interval and one-off timers generate an 'event' on expiration. The event is simply
a structure that contains some internal details, primarily the lua callback associated
with this event that must be called back as a result of the timer having expired.
This event gets added to a global event queue in this library.

**At no point is LUA called back asynchronously.**

The queue simply gets populated, from multiple threads (and therefore enqueing
and dequeing operations must be and are protected by a mutex to ensure serialization).

The lua script itself must periodically call `luamockit.process_events()`
which will go through the event queue and dequeue each pending event and 'handle' it.
To handle an event means to call the Lua callback associated with it, and then
remove it from the event queue.

Therefore each call to `luamockit.process_events()` has a backlog that it needs to clear.
The more rarely `luamockit.process_events()` gets called, the bigger the backlog and the 
less precise the timers will seem.

That is to say, if the user registers a callback to be called back every 3 seconds
but the `luamockit.process_events()` gets called every 10 seconds, then the callback will
only get called every 10 seconds and every 10 seconds it will be called 3 times
in a row (because the timer for the callback will have expired 3 times)!

It's therefore important to call the `luamockit.process_events()` function as often as
possible : or, specifically, about as frequently as the callback with the shortest
interval timer.

To solve this problem another function is provided - `luamockit.wait()` -
which makes a blocking call to wait on the list to be populated. It unblocks as
soon as an event is added to the list. Note that if there are events in the queue,
this function still blocks and will only unblock when a **NEW** event gets generated.
It's therefore advisable that the user either call this function before any events
get created (i.e. before setting up any timers) or after emptying the queue with
`luamockit.process_events()`. With the above in mind, a possible undesirable scenario
is that the function gets called after some events have already been generated
but without any new ones being generated at anytime, with the result that the function
will block forever and the events already in the queue end up never being handled. 
To account for this possibility, `luamockit.wait()` takes an optional parameter, 
`timeout`. The blocking call will unblock either as soon as a new event is generated or 
after timeout milliseconds -- whichever comes first.

### Lua script design

The above points mean that a certain design naturally imposes itself for a lua script
using the interval timers feature of this library (though this is by no means mandatory).
The script will most often take the form of an infinite loop that blocks with
`luamockit.wait()` and then calls `luamockit.process_events()` every time it unblocks.

This makes it most suited to scripts meant to be running as daemons. This might
not be as much of a problem as it sounds, since that's exactly the context where
one would typically even want to have callbacks called asynchronously or at certain
intervals: event loops dispatching events.

-----------------------------------------------------------------------

## Luamockit Usage Overview

This section will give an overview of how one would use this library
in a Lua script. For more details (e.g. about the return values of
functions and such) see `src/luamockit.c`, which has ample comments,
and for more examples, see `tests/tests.lua`.

The code below assumes `luamockit.so` is already on a standard path
(i.e. in Lua's `cpath`. Otherwise the user can set `package.cpath` in
the script or `LUA_CPATH` in the environment, as appropriate).

```Lua
local luamockit = require("luamockit")

-- get an integral millisecond timestamp
local ts = luamockit.mstimestamp()   -- e.g. 1656970659385

-- get a time tuple of seconds since the epoch and milliseconds
-- since the last second
local s,ms = luamockit.time()
if not s then print("time failed with error code " .. ms)

-- sleep for 3.5 seconds and resume on signal interrupts
luamockit.sleep(3500, true)

-- sleep for 0.7 seconds and do not resume on signal interrupts
local error_code, time_left = luamockit.sleep(700)

-- call a callback in 11 seconds
-- 11 seconds can be spent either sleeping, handling other tasks
-- or blocking with luamockit.wait() until something gets added to the
-- event queue
function cb()
    print(string.format("called callback at %s", luamockit.mstimestamp()))
end

luamockit.oneoff(11*1000, cb)
luamockit.wait(15*1000)          -- block for 15 seconds at the most 
luamockit.process_events()       -- process all events in the event queue

---------------------------------------------------------------

-- set up two interval callbacks: one every 7 seconds, one every 0.07
-- seconds
local t1 = luamockit.getit(7 * 1000, cb)
local t2 = luamockit.getit(70, cb)
while true do
  luamockit.wait()
  luamockit.process_events()
end

-- NOTE:
-- to limit how long to process events for the user can use a simple
-- loop-break condition, for example `while luamockit.mstimestamp() < endtime`
-- where endtime is another mstimestamp marking a deadline for the loop.

-- when finished, destroy mockit timer objects by assigning all referring
-- variables to nil to allow lua to garbage collect the objects
t1 = t1:destroy()   -- disarms timer and returns nil
t2:destroy()
t2 = nil

```


## TODOs
    * increase fixed-width int type used from 32 to 64 bits


## Luamockit design

![luamockit architecture design](/docs/luamockit.drawio.svg "luamockit architecture design")

