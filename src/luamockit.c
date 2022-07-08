/* ===================================================================================
   * * * * * * * * * * * * DESIGN AND IMPLEMENTATION NOTES * * * * * * * * * * * * * * 
   -----------------------------------------------------------------------------------
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

   If using coroutines, each one has its own lua thread, and a C function that gets
   called might therefore be called with a lua_State other than the main one.
   Instinctively, one would would think to save this state in a C data structure 
   and then use it to call back into lua on a timer's expiry, when a registered
   callback must be called. However, the state might've well disappeared by then - 
   resulting in a crash.
   To ensure the lua threads don't go anywhere and avoid the aforementioned issue,
   they must be ANCHORED, either via assignment to a lua variable or by saving them
   in the lua REGISTRY.
   
   USING A SINGLE GLOBAL STATE
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
   Mockit_register_oneoff_callback() functions get called with, and therefore CAN 
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
   libraries etc -- which is slower. It's also very cumbersone to share variables
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

   the Lua interpreter itself has global locks one can use, but it's not an option if
   one wants or must use the standard lua version provided.

   ------------------------------------------------------------------------------------
   -------------------------------> EVENT QUEUE <--------------------------------------
   ------------------------------------------------------------------------------------
   The approach this library ultimately settled on is all about giving the power to lua
   itself.
   Interval and one-off timers generate an 'event' on expiration. The event is simply 
   a structure that contains some internal details, primarily the lua callback associated
   with this event that must be called back as a result of the timer having expired. 
   This event gets added to a global event queue in this library.

   * At no point is LUA called back asynchronously. *

   The queue simply gets populated, from multiple threads (and therefore enqueing
   and dequeing operations must be and are protected by a mutex to ensure serialization).

   The lua script itself must periodically call the lua_process_events() function
   which will go through the event queue and dequeue each pending event and 'handle' it.
   To handle an event means to call the Lua callback associated with it, and then
   remove it from the event queue.
    
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

   To solve the above problem another function is provided - `lua_wait_for_events()` - 
   which makes a blocking call to wait on the list to be populated. It unblocks as
   soon as an event is added to the list. Note that if there are events in the queue,
   this function still blocks and will only unblock when a NEW event gets added.
   It's therefore advisable that the user either calls this function before any events
   get created (i.e. before setting up any timers) or after emptying the queue with
   `lua_process_events()`. With the above in mind, a possible undesirable scenario
   is that the function gets called after some events have already been generated
   but without any new events being generated, with the result that the function
   will block forever and the events end up never being handled. To account for this
   possibility, `lua_wait()` takes an optional parameter, `timeout`. The blocking call
   will unblock either as soon as a new event is generated or after timeout milliseconds
   -- whichever comes first.
   
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
   ===================================================================================
 */

#include <errno.h>
#include <string.h>          // strerror()
#include <assert.h>
#include <stdlib.h>          // exit()
#include <semaphore.h>       // named and unnamed POSIX semaphores
#include <time.h>            // clock_gettime()

#include <lua5.3/lua.h>
#include <lua5.3/lauxlib.h>
#include <lua5.3/lualib.h>

#include "mockit.h"
#include "queue.h"

// the index in the Lua registry where the Lua callback is stored
// that must be called for a particular timer expiration;
// and the associated lua state associated with the callback and timers
#define LUA_CB_IDX_IN_LUA_REGISTRY    0
#define LUA_STATE_IDX_IN_LUA_REGISTRY 1

// metatable in Lua for interval timer object ('mockit')
#define MOCKIT_MT "mockit_mt__"


//================================
// ----- Type definitions ------
//================================

/* 
 * an event in this context is an entry in an event queue, associated 
 * with (and added as a result of the expiry of a) timer (either 
 * one-shot or an interval timer).
 *
 * Each event points to a `struct data`, which in turn, among others, 
 * contains a callback. Dequeing and 'handling' the event means removing
 * it from the event queue and calling its callback function.
 */
typedef struct qi event_t;


//================================
// ------ File-scoped vars -------
//================================

/*
 * 'events' (dynamically allocated `struct event` types) get put in this
 * 'event' queue. An event is added when created, and removed when handled.
 */
static struct queue equeue = {.head = NULL, .tail = NULL};

//================================
// ----- Function definitions ----
//================================

/* defined in mockit.c */
extern void timespec_add_ms__(struct timespec *ts, uint32_t ms);

/*
 * Initialize the Lua library before it can be used.
 *
 * Initialization involves:
 * - initializing the unnamed semaphore `esem`.
 *
 * <-- return 
 *     0 if successful, else the errno value set by one of the
 *     functions called by queue_init: sem_init, if queue_init
 *     returns 1; pthread_mutex_init, if queue_init return 2.
 */
int lua_initialize(void){
    errno = 0;

    if (queue_init(&equeue, true, true)) return errno;
    if (Mockit_init()) return errno;
    return 0;
}

/*
 * Increment the `esem` semphore to signal the creation of a new event.
 *
 * When a new 'event' is created, it's added to the event queue and the 
 * semaphore is incremented (posted).
 *
 * <-- return
 *     0 on success, else the errno value set by sem_post() on failure.
/
int signal_event__(void){
    int res = 0;
    
    res = sem_post(&esem);
    if (res == -1){
        return errno;
    }

    return 0;
}
*/

/*
 * Wait for a new event to be added to the event queue.
 *
 * This is done by making a call to decrement (wait) the 
 * `esem` semaphore that's posted by signal_event().
 * The call made is of course blocking, and it unblocks 
 * as soon as the value of `esem` is > 0.
 *
 * --> timeout
 *     If timeout is 0, a call to sem_wait() is made to block
 *     indefinitely waiting for a semaphore post on esem.
 *     Otherwise if timeout > 0, it should be a timeout value
 *     in milliseconds for how long to block waiting for a semaphore
 *     post. A call to sem_timedwait() is made instead of sem_wait().
 *
 * <-- errnum
 *     This is unused if timeout is 0. Otherwise if timeout > 0, calls are
 *     made to various functions as explained next in the `return` section.
 *     These functions set `errno`. If `errnum` is not NULL, `wait_for_event__`
 *     will write to `errnum` the value of `errno` as set by any of the 
 *     functions that failed. `errnum` can then be looked at in conjunction with
 *     the value returned by `wait_for_event` to discern which function
 *     set errno. If the value returned by `wait_for_event` is 0, errnum 
 *     should be ignored.
 *
 * <-- return
 *     0 on success, else 1 if clock_gettime() failed, else 2 if
 *     sem_timedwait() failed. If errnum is not NULL, the errno value set by 
 *     clock_gettime() or sem_timedwait() is written there.
 */
int wait_for_event__(struct queue *equeue, uint32_t timeout, int *errnum){
    errno = 0;

    if (!timeout){
        // do not consider signal interrupts errors
        if (sem_wait(&equeue->sem) == -1 &&  errno != EINTR){
            if (errnum) *errnum = errno;
            return 2;
        }       

        return 0;
    }

    // else timeout > 0
    struct timespec timespec;
    memset(&timespec, 0, sizeof(struct timespec));

    if (clock_gettime(CLOCK_REALTIME, &timespec) == -1){
        if (errnum) *errnum = errno;
        return 1;
    }

    timespec_add_ms__(&timespec, timeout);

    // do not consider timeouts or interrupts erros
    if (sem_timedwait(&equeue->sem, &timespec) == -1 
            && errno != ETIMEDOUT
            && errno != EINTR)
    {
        if (errnum) *errnum = errno;
        return 2;
    }

    return 0;
}

/*
 * Handle each event in the event queue.
 *
 * This is the function that actually processes the 'events'
 * in the event queue. This is meant to be called from within LUA. 
 * The 'events' here are in essence just timer expirations, either 
 * one-off or periodic. Each event object is a wrapper for some
 * data, including a reference into the lua registry to a lua callback 
 * function that must be called when the event is dequeued/ 'handled'. 
 * The callback IS really the whole point of the 'event' in this library.
 * NOTE: the Lua callback function must take no params and return no values.
 * 
 * The way this gets called in LUA is typically after the `esem` semaphore
 * is posted in this library, which in turn unblocks the blocking call made 
 * from within Lua to wait for events.
 *
 * <-- return @Lua
 *     An error is thrown in Lua in case of failure; 
 *     the number of events handles is returned otherwise.
 */
int lua_process_events(lua_State *L){
    event_t *event = NULL;
    struct data *data   = NULL;
    lua_Integer num_handled = 0;

    // lua callback must take no arguments : discard everything
    lua_settop(L,0);

    while ((event = qi_dequeue(&equeue))){
        data = event->data;
        lua_State *Lstate = data->ctx;

        lua_rawgeti(Lstate, LUA_REGISTRYINDEX, data->refs[LUA_CB_IDX_IN_LUA_REGISTRY]);
        if (!lua_isfunction(Lstate, 1)){
            luaL_error(Lstate, "failed to retrieve callback from Lua registry");
        }
        // lua callback function must take no params and return no values
        lua_pcall(Lstate, 0, 0, 0);

        // data should ONLY be deallocated if the current event is NOT
        // the result (of the expiration) of an interval timer; that's 
        // because in the case of interval timers, a userdata object is 
        // returned to lua that represents that interval timer, when created.
        // This would have to be manually and explicitly deallocated by 
        // removing all references from lua and by calling the destroy() metamethod. 
        // Lua takes care of all of that (you actually CANNOT call free() on 
        // Lua-managed userdata).
        // One-off timers otoh (the only other possible type of event here) do not
        // return a userdata upon creation and so they must be deallocated here
        if (!data->is_cyclic__){ 
            // unref Lua callback and luastate from lua registry
            luaL_unref(Lstate, LUA_REGISTRYINDEX, data->refs[LUA_CB_IDX_IN_LUA_REGISTRY]); 
            luaL_unref(Lstate, LUA_REGISTRYINDEX, data->refs[LUA_STATE_IDX_IN_LUA_REGISTRY]); 
            free(data);
            qi_destroy(event, false);
        }

        ++num_handled;
    }

    lua_pushinteger(L, num_handled);
    return 1;
}

/*
 * Block until an event is posted to the event queue.
 *
 * The blocking call is achieved by WAITING on the `esem`
 * semaphore, which is incremented with every new event
 * generated.
 *
 * --> timeout
 *     an optional timeout value in milliseconds to block for. After TIMEOUT
 *     milliseconds this function will return regardless of whether any new 
 *     events have been added to the event queue or not.
 *
 * <-- return @lua
 *     1 on failure (e.g. sem_wait() failed), 0 on success.
 */
int lua_wait_for_events(lua_State *L){
    uint32_t timeout = 0;

    // the timeout arg from lua is optional. See the comments on 
    // wait_for_event__() for the semantics of this argument
    lua_settop(L,1);
    if (lua_type(L, 1) != LUA_TNIL){   // 1 argument, it must be an integer
        luaL_checktype(L,1,LUA_TNUMBER); 
        int res = 0;
        timeout = lua_tointegerx(L, 1, &res);
        if(!res) luaL_error(L,"failed to get timeout arg: must be an integer");
    }

    if (wait_for_event__(&equeue, timeout, NULL)){     // failed
        lua_pushinteger(L, 1);
    }
    else{
        lua_pushinteger(L, 0);
    }

    return 1;
}

static inline void add_event_to_queue__(void *data){
    qi_enqueue(&equeue, data, true);
}

/*
 * Create an interval timer that calls callback on interval expiration.
 *
 * This function creates an interval timer that will expire every
 * INTERVAL milliseconds. The first expiration occurs INTERVAL milliseconds 
 * from NOW. On each expiration, an 'event' is added to the
 * global event queue (`equeue`) which periodically gets processed (ideally 
 * as often as possible to ensure greater time precision) periodically
 * by lua_process_events (called from within lua), which dequeues and
 * handles every event in the queue until it's empty. 
 * Each event added represents a callback that needs to be called back,
 * registered from within a lua script by passing it as the 2nd param
 * to this function. 
 *
 * The result of this function, in Lua, is a `struct data` userdata.
 * This MUST be assigned to a variable, in that it identifies the
 * associated interval timer created, which needs to be called 
 * destroy() on when no longer needed / when the user wants to disable
 * it.
 *
 * Therefore this function must be called like this:
 *    local timer_obj = luamockit.this_function(5,mycallback)
 * and then, when timer_obj is no longer needed: 
 *    timer_obj = timer_obj:destroy() // see the timer_destroy() function below
 *
 *
 * The lua thread from within which this function got called is saved and the
 * asssociated event (timer expiration) will call back into lua using this same
 * thread (NOTE: lua thread # POSIX thread); 
 * The thread/state is anchored in the registry to ensure it doesn't disappear (it does
 * not get garbage-collected).
 * However, since correct usage of this library normally consists of an infinite loop
 * (see "Design Notes") mostly sleeping and periodically calling lua_process_events()
 * or spending most of its time waiting on semaphore signalling in lua_process_events(),
 * it's NOT expected that multiple Lua threads will be used.
 * Although this library will, as mentioned, use the tlua thread it was called from,
 * correct behavior is not guaranteed if multiple threads are in actuality used.
 *
 * --> interval @lua
 *     interval value in milliseconds to wait before calling the registered callback.
 *
 * --> Lua function @lua
 *     a callback to call on timer expiration. This must take no arguments.
 *
 * <-- return @lua
 *     a `struct data` userdata to be assigned to a variable in Lua.
 *
 */
int Mockit_get_interval_timer(lua_State *L){
    int res = 0;

    // check arguments to this function passed from Lua
    lua_settop(L,2);
    luaL_checktype(L,1,LUA_TNUMBER);
    luaL_checktype(L,2,LUA_TFUNCTION);

    lua_Integer timeout = lua_tointegerx(L,1,&res);
    if(!res){
        luaL_error(L,"failed to get timeout arg: must be an integer");
        return 1;
    }

    // save Lua callback and lua thread in the Lua registry
    // they'll be retrieved from there when the callback gets
    // called on timer expiration
    int lua_callback_ref = luaL_ref(L,LUA_REGISTRYINDEX);
    if(lua_callback_ref == LUA_REFNIL){
        luaL_error(L,"failed to save Lua callback in the Lua registry");
        return 1;
    }
    lua_pushthread(L);
    int lua_state_ref = luaL_ref(L,LUA_REGISTRYINDEX);
    if(lua_state_ref == LUA_REFNIL){
        luaL_error(L,"failed to save lua thread in the Lua registry");
        return 1;
    }
 
    // else, we successfully got the refereces
    struct data *dt = lua_newuserdata(L, sizeof(struct data));
    if (!dt){
        luaL_error(L,"Failed to allocate memory for data struct");
        return 1;
    }
    dt->refs[LUA_CB_IDX_IN_LUA_REGISTRY]    = lua_callback_ref; // unique lua registry reference
    dt->refs[LUA_STATE_IDX_IN_LUA_REGISTRY] = lua_state_ref;
    dt->ctx = L; // used to pass the lua_State to callback
    dt->timeout = (uint32_t) timeout;
    dt->cb = add_event_to_queue__;
    dt->free_ctx = false;
    dt->free_data = false;

    if((res = Mockit_getit(dt->timeout, dt))){
        luaL_error(L,"failed to set up interval timer: %s", strerror(res));
        return 1;
    }

    res = luaL_getmetatable(L, MOCKIT_MT);
    if (!res){ luaL_error(L, "failed to find metatable for mockit object"); return 1; }
    lua_setmetatable(L, -2); // set MOCKIT_MT as the userdata's metatable
    lua_insert(L,1);         // the userdata -- move it to index 1 and
    lua_settop(L,1);         // pop everything else off the stack

    // userdata is recognized as being of the correct type, i.e. not another
    // library's userdata, by checking if it has the MOCKIT_MT metatable, which
    // is unique to this library's userdata
    luaL_checkudata(L,1,MOCKIT_MT);

    return 1; // returns userdata
}

/*
 * Register a one-off callback from a lua script i.e. the callback 
 * registered only gets called back ONCE.
 *
 * --> timeout, @lua
 *     The duration to wait for, in milliseconds, before calling the callback.
 * 
 * --> lua function, @lua
 *     a callback lua function that takes no arguments.
 *
 * <-- return, @lua
 *     Calling this function in LUA does NOT return anything, therefore, unlike 
 *     the function for creating an interval timer, the return value of this 
 *     function (no return value) doesn't need to be assigned to a variable and 
 *     therefore the destroy() metamethod CANNOT be called on it because the user
 *     would be calling it on nil.
 *
 * A One-off callback is implemented by sleeping for a period of time
 * in a thread (in the mockit library), which amounts to the timeout value,
 * and then adding an event to the global event queue here, on wakeup.
 * The callback gets called when lua_process_events() gets around
 * to processing the respective event in the queue. That is to say, even
 * if the event is generated on time, if the user does not process the 
 * respective event for 30 minutes, then no callback will be called until then!
 */
int Mockit_register_oneoff_callback(lua_State *L){
    int res = 0;

    // check Lua arguments to this function
    lua_settop(L,2);
    luaL_checktype(L,1,LUA_TNUMBER);   // interval value
    luaL_checktype(L,2,LUA_TFUNCTION); // callback function

    uint32_t timeout = (uint32_t) lua_tointegerx(L,1,&res);
    if(!res){
        luaL_error(L,"failed to get interval value: must be an integer");
    }

    // see notes about Lua thread/state under Lua_get_interval_timer
    int lua_callback_ref = luaL_ref(L,LUA_REGISTRYINDEX);
    if(lua_callback_ref == LUA_REFNIL){
        luaL_error(L,"failed to save lua callback in the Lua registry");
    }

    lua_pushthread(L); // push thread/state
    int lua_state_ref = luaL_ref(L, LUA_REGISTRYINDEX); // create reference to state/thread (i.e. ANCHOR it)
    if(lua_state_ref == LUA_REFNIL){
        luaL_error(L,"failed to save lua state in the Lua registry");
    }
    
    // allocate and initialize a data object that the callback will be called with
    struct data *dt = Mockit_dynamic_data_init(add_event_to_queue__, timeout, L, false, false);
    if (!dt){
        luaL_error(L,"Failed to allocate memory for data struct");
    }
    dt->refs[LUA_CB_IDX_IN_LUA_REGISTRY]    = lua_callback_ref;// unique lua registry ref to callback
    dt->refs[LUA_STATE_IDX_IN_LUA_REGISTRY] = lua_state_ref;   // unique lua registry ref to lua state/thread for anchoring
    dt->free_data = false;
    dt->free_ctx = false;

    // the sleep happens in a different thread so that the library isn't blocked and 
    // can deal with the registration of more timers and callbacks; this means we get a 
    // value for res and can return to lua immediately as opposed to having to wait for 
    // the sleep to finish
    if(Mockit_oneoff(timeout, dt)){
        luaL_error(L,"failed  to register one-off callback");
    }

    return 0;
}

/*
 * Destroy an interval timer ('mockit' object.)
 *
 * --> mockit object, @lua
 *     the struct data userdata representing the timer object ('mockit')
 *     to destroy. 
 *
 * <-- return
 *     nil: this should be assigned back to the timer object being destroyed.
 *
 * This is a metamethod to be called in lua like this:
 *    timer_obj = timer_obj:destroy()
 * where timer_obj holds the return value of a call
 * to lua_get_interval_timer() made previously, e.g.
 *    timer_obj = mockit.lua_get_interval_timer(10, mycallback)
 *
 * This metamethod must ONLY be called on a value returned by
 * lua_get_interval_timer(), since lua_get_oneoff_timer() returns
 * no result and therefore need not, cannot and must not be called 
 * destroy() on.
 *
 * For interval timer objects, the result of this function MUST be
 * assigned back to the variable holding the object the user is 
 * trying to destroy. That's because the object being destroyed is
 * a userdata, which means its memory is handled by lua, and it can't
 * be garbage collected until all references to it are gone. 
 * the destroy() metamethod returns nil, and assigning that back
 * to the variable holding the userdata will therefore remove
 * all references and makes it so that it can be garbage collected.
 *
 * This of course assumes that is the ONLY reference to the respective
 * userdata / timer object. Therefore the user MUST not have multiple
 * variables pointing to the same object. Each call to 
 * lua_get_interval_timer() MUST be assigned to a single unique variable
 * in lua that destroy() is then called on and its return value assigned 
 * back to. Otherwise the user must make sure all references (Lua variables
 * pointing said interval timer object / userdata) are set to nil when
 * the interval timer is no longer needed and its garbage collection is
 * desired.
 */
int destroy_interval_timer(lua_State *L){
    // check arguments in Lua
    lua_settop(L, 1);
    luaL_checkudata(L,1, MOCKIT_MT);
    struct data *dt = lua_touserdata(L,1);
    if (!dt){  // NOT a userdata, invalid argument
        luaL_error(L, "Invalid function argument: not a userdata (mockit object expected)");
    }
    // can't call free on userdata as that's lua-managed memory;
    // simply destroy the timer id instead and return nil 
    // so as to remove any reference to this (see comments above) and 
    // then Lua will garbage collect the memory
    int res = Mockit_disarm(dt->thread_id__);  
    printf("resss is %i\n", res);
    //lua_pushnil(L); // the result of this function should be assigned back to the timer object
    //return 1;
    return 0;
}

/*
 * Makes a BLOCKING call to nanosleep() (called in the mockit library).
 *
 * --> duration
 *     the amount of time to sleep in milliseconds.
 * 
 * --> do_restart
 *     True if the sleep should be restarted/resumed after a signal interrupt.
 *     False if on any interruption the call should give up and return the 
 *     number of milliseconds left until sleep would have been completed.
 *
 * <-- remaining
 *     time left to sleep. If do_restart is true, sleep always completes,
 *     so this value will always be 0.
 * 
 * <-- return 
 *     0 on sucess, an error number on error i.e. a tuple of (error code, remaining)
 *     is returned, which is (0,0) on success, and (errno, time_left) on error.
 */
int luasleep(lua_State *L){
    int res = 0;

    // check arguments in Lua
    lua_settop(L,2);
    luaL_checktype(L, 1, LUA_TNUMBER);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    uint32_t sleep_time = (uint32_t) lua_tointegerx(L,1,&res);
    if (!res){
        luaL_error(L, "incorrect time value specified for sleep duration.");
    }
    bool restart_sleep = lua_toboolean(L, 2);

    uint32_t remaining = 0;
    int error_code     = 0;
    
    if ((error_code = Mockit_bsleep(sleep_time, restart_sleep, &remaining))){
        //luaL_error(L, "Call to sleep failed : %s", strerror(error_code));
    }
    
    lua_pushinteger(L, remaining);
    lua_pushinteger(L, error_code);
   
    return 2;   // returns time left to sleep, error code
}

/*
 * Return a time tuple of (seconds, milliseconds) on success
 * or a tuple of (nil, error code) on failure.
 *
 * The error code on failure is the value of errno set by
 * clock_gettime() (see the Mockit_gettime() comments in 
 * mockit.h).
 *
 * <-- return, @lua
 *     Number of seconds as a Unix timestamp
 *
 * <-- return, @lua
 *     millieseconds since the last second.
 */
int get_time_tuple(lua_State *L){
    time_t secs;
    long ms;
    int res = 0;

    // failure
    if (( res = Mockit_gettime(&secs, &ms))){
        lua_pushnil(L); 
        lua_pushinteger(L, res);
    }
    // success
    else{
        lua_pushinteger(L, secs);
        lua_pushinteger(L, ms);
    }

    return 2;
}

/*
 * Get a millisecond timestamp since the Epoch.
 * See Mockit_mstimestamp() fmi.
 *
 * <-- return, @lua
 *     A millisecond timestamp since the Unix Epoch.
 */
int get_mstimestamp(lua_State *L){
    uint64_t timestamp = 0;
    if (!Mockit_mstimestamp(&timestamp)){
        lua_pushinteger(L, timestamp);
    }
    else{
        lua_pushnil(L);
    }

    return 1;
}


//==================================================================
//--------- Lua library configuration and initialization -----------
//==================================================================

/* Module functions */
const struct luaL_Reg luamockit[] = {
    {"oneoff", Mockit_register_oneoff_callback},
    {"sleep", luasleep},
    {"time", get_time_tuple},
    {"mstimestamp", get_mstimestamp},
    {"getit", Mockit_get_interval_timer},
    {"process_events", lua_process_events},
    {"wait", lua_wait_for_events},
    {NULL, NULL}
};

/* Module metamethods */
const struct luaL_Reg luamockit_metamethods[] = {
    {"destroy", destroy_interval_timer},
    {NULL, NULL}
};

/* Open/initialize module */
int luaopen_luamockit(lua_State *L){
    assert(lua_initialize() == 0);            // initialize semaphore etc
    luaL_newmetatable(L, MOCKIT_MT);
    lua_pushvalue(L,-1);
    lua_setfield(L,-2, "__index");            // the metatable's __index method should point to the metatable itself
    luaL_setfuncs(L,luamockit_metamethods,0); // populate MOCKIT_MT with the metamethods in mockits_metamethods

    luaL_newlib(L, luamockit);
    return 1;
}
