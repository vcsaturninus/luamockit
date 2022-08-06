#include <errno.h>
#include <string.h>          // strerror()
#include <assert.h>
#include <stdlib.h>          // exit()
#include <semaphore.h>       // unnamed POSIX semaphores
#include <time.h>            // clock_gettime()

#include <lua5.3/lua.h>
#include <lua5.3/lauxlib.h>
#include <lua5.3/lualib.h>

#define LUAMOCKIT
#include "mockit.h"
#include "morre.h"
#include "common.h"

#ifdef DEBUG_MODE
extern int DEBUG;
#endif


/* metatable in Lua for interval timer object ('mockit');
 * exposed to lua code as a full userdata
 */
#define MOCKIT_MT "mockit_mt__"

/* Timeout value when waiting for a MOCKIT_MOD-marked event */
#define LUAMOCKIT_MOD_WAIT_TIMEOUT 5000


/* ================================ *
 * ----- Struct definitions ------  *
 * ================================ */

/*
 * an event in this context is an entry in an event queue, associated
 * with (and added as a result of the expiry of a) timer (either
 * one-shot or an interval timer).
 *
 * Each event points to a `struct mockit`, which among others,
 * contains a pointer to a `struct luamockit`. This in turn has pointers
 * to a Lua callback, the Lua state, and various references in the Lua
 * Registry used for anchoring purposes. Dequeing and 'handling' the event
 * means removing it from the event queue and calling its callback function.
 *
 * When an interval timer is disarmed, a mark is implicitly set (MOCKIT_MFD)
 * in the struct representing the timer object. The thread in charge of the
 * timer will set another mark (MOCKIT_MOD) in acknowledgement. Any event
 * struct marked with MOCKIT_MFD is ignored: the associated Lua callback is
 * NOT called anymore because the timer has been _disarmed_. However the
 * resources associated with the timer cannot be released yet either: the timer
 * _cannot_ self-destruct and clean up because there may be events in the queue
 * holding references to those resources and therefore it's a distinct possibility
 * use-after-free will be performed by the event queue processor, potentially leading
 * to a crash. Conversely, a timer cannot be destroyed from the queue event processor
 * at any given time because then the thread itself that's in charge of the  timer
 * may perform use-after-free and/or generate new events that will need to be handled
 * (and handling would be impossible since the Lua references etc will've been released
 * by the event queue processor, as mentioned).
 *
 * The sensible and correct way of releasing the resources requires coordination.
 * This is achieved through the 'marks' aforementioned. When the user wants to
 * destroy a timer, first the timer gets 'disarmed', which implicitly sets the
 * MOCKIT_MFD mark. The timer thread wil see this as soon as it wakes up and set
 * MOCKIT_MOD in acknowledgement, then generate a last MOCKIT_MOD-marked event before
 * definitely exitting. The event queue processor knows an event is essentially expired
 * when it seen the MOCKIT_MFD mark set, so the event is accordinly ignored. The next
 * event associated with the timer must be MOCKIT_MOD-marked, on receipt of which the
 * event queue processor releases all resources (e.g. any dynamic memory _not_ owned by
 * Lua, lua references etc) associated with the timer. At this point the timer is
 * considered destroyed, so we can finally return to Lua (which has been blocking on
 * the destroy call) safely.
 *
 * Interval timers are therefore a stream made up of any number of unamarked
 * events terminated with MOCKIT_MOD-marked event preceded by a MOCKIT_MFD-marked
 * event. A one-shot timer otoh always generates a single MOCKIT_MOD-marked event.
 * MOCKIT_MFD is never set for one-off timers because they never get disarmed.
 *
 * Note each event container/struct MUST have a copy of the mark of the timer AS 
 * IT WAS AT THE TIME THE EVENT WAS GENERATED. This allows each event struct to be
 * marked and examined independently as each would have its own copy of the mark.
 * Conversely, it's perilous to use the timer mark only ince if changed at any point
 * in time, since the timer object is being pointed to by every event struct, its 
 * mark will be changed even in event structs that have already been generated.
 * For example, setting MOCKIT_MOD would cause the fatal problem of multiple event
 * structs now pointing to a timer object whose mark has MOD set. Having each event
 * struct maintain its own copy of the mark avoids such thorny issues.
 */
struct event{
    void *data;
    struct event *next;
    uint8_t mark;  /* a copy of the timer object (carried in data) mark */
    bool cyclic;   /* true if carrying a cyclic timer payload */
};

/*
 * 'events' (dynamically allocated `struct event` types) get put here.
 * An event is added when created, and removed when handled.
 */
struct event_queue{
    struct event *head, *tail;
    uint64_t count;   /* all entries in the queue, indiscriminately */
    uint64_t pending; /* only pending events, excluding ones that have a mark set */
};

/*
 * Extends the timer object with attributes needed for Lua integration */
struct luamockit{
    int lua_state_ref; /* index in the Lua registry to Lua state reference */
    int lua_cb_ref;    /* index in the Lua registry to a Lua callback */
    int lua_udata_ref; /* index in the Lua registry to lua-allocated `struct mockit` */
    void *lua_state;   /* pointer to the lua state to call back into; anchored via lua_state_ref */
};


/* ================================ *
 * ----- File-scoped vars --------  *
 * ================================ */

/*
 * Queue to hold timer expiration events for Lua.
 */
static struct event_queue equeue = {.head = NULL, .tail = NULL};

// used to serialzie enqueueing and dequeueing operations
static pthread_mutex_t qmtx = PTHREAD_MUTEX_INITIALIZER;

// event semaphore used to signal the addition of a new event to the queue
static sem_t esem;


/* ================================ *
 * ----- Function definitions ----  *
 * ================================ */

/* defined in mockit.c */
extern void timespec_add_ms__(struct timespec *ts, uint32_t ms);
extern uint8_t Mockit_setmod(uint8_t mark);
extern bool Mockit_ismfd(uint8_t mark);
extern bool Mockit_hasmod(uint8_t mark);

/*
 * Used to ensure that before the thread in charge of the timer exits
 * a last MOCKIT_MOD-marked event is generated so that the event queue
 * procesor knows to release all resources associated with the timer.
 *
 * The sensitive cleanup and resource release operations are left to the
 * event queue processor.
 */
static int mockit_cthread_finalizer(void *mit){
    assert(mit);
    struct mockit *m = mit;
    

    /* for one-off timers simply return; the point of the finalizer here
     * is to stop mockit's default behavior of calling free() */
    if (!m->is_cyclic__) return 0;

    unsigned int mark = m->mark__ ;
    (void) mark;
    m->mark__ = Mockit_setmod(m->mark__);
    say(DEBUG, "finalizer : setting MOD for event with ival=%u, mit %p , lmit  %p; before - %u, after - %u\n", ((struct mockit*)mit)->timeout__, (void *)mit, (void *)m->ctx, mark, m->mark__);
    m->cb(m);

    return 0; /* success */
}

void remove_lua_refs(struct luamockit *lmit, bool cyclic){
    assert(lmit);
    lua_State *Lstate = lmit->lua_state;

    // unref Lua callback and lua state from lua registry
    // cyclic timers also have a reference in the Lua registry
    // in order to keep the struct mockit anchored once the user has
    // disarmed the full user data
    // fprintf(stderr, "--> interval : removing reference to userdata , mit=, mark=\n");
    luaL_unref(Lstate, LUA_REGISTRYINDEX, lmit->lua_cb_ref);
    luaL_unref(Lstate, LUA_REGISTRYINDEX, lmit->lua_state_ref);

    if (cyclic){
        luaL_unref(Lstate, LUA_REGISTRYINDEX, lmit->lua_udata_ref);
    }
}

/*
 * Initialize the Lua library before it can be used.
 *
 * Initialization involves:
 * - initializing the unnamed semaphore `esem`.
 * - initializing the event queue mutex `qmtx`.
 *
 * <-- return
 *     0 if successful, else 1 if the semaphore
 *     could not be initialized, else 2 if the mutex
 *     could not be initialized. If errnum is not NULL,
 *     the value of errno is stored there as set by
 *     any failed function call (e.g. sem_init()). On
 *     success any value written into errnum should be
 *     ignored.
 */
int lua_initialize(int *errnum){
#ifdef DEBUG_MODE
    if (getenv("DEBUG_MODE")) DEBUG = 1;
#endif

    errno = 0;

    if (sem_init(&esem,0,0)){
        if (errnum) *errnum = errno;
        return 1;
    }

    if (pthread_mutex_init(&qmtx, NULL)){
        if (errnum) *errnum = errno;
        return 2;
    }

    errno = 0;
    if (errnum) *errnum = 0;

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
 */
int signal_event__(void){

    if (sem_post(&esem) == -1){
        return errno;
    }

    return 0;
}

/*
 * Wait for a new event to be added to the event queue.
 *
 * This is done by making a call to decrement (wait) the
 * `esem` semaphore that's posted by signal_event__().
 * The call made is of course blocking, and it unblocks
 * as soon as the value of `esem` is > 0.
 *
 * --> timeout
 *     If timeout is 0, a call to sem_wait() is made to block
 *     indefinitely waiting for a semaphore post on esem.
 *     Otherwise if timeout > 0, it should be a timeout value
 *     in milliseconds for how long to block waiting for a semaphore
 *     post; a call to sem_timedwait() will be made instead of sem_wait().
 *
 * <-- errnum
 *     This is unused if timeout is 0. Otherwise if timeout > 0, calls are
 *     made to various functions as explained next in the `return` section.
 *     These functions set `errno`. If `errnum` is not NULL, `wait_for_event__()`
 *     will write into `errnum` the value of `errno` as set by any of the
 *     functions that failed. `errnum` can then be looked at in conjunction with
 *     the value returned by `wait_for_event()` to discern which function
 *     set errno. If the value returned by `wait_for_event()` is 0 (success),
 *     errnum should be ignored.
 *
 * <-- return
 *     0 on success, else 1 if clock_gettime() failed, else 2 if
 *     sem_timedwait() failed. If errnum is not NULL, the errno value set by
 *     clock_gettime() or sem_timedwait() is written there.
 */
int wait_for_event__(uint32_t timeout, int *errnum){
    errno = 0;
    if (errnum) *errnum = 0;

    if (!timeout){
        // do not consider signal interrupts errors
        if (sem_wait(&esem) == -1 && errno != EINTR){
            if (errnum) *errnum = errno;
            return 2;
        }

        return 0;
    }

    // else timeout > 0
    struct timespec timespec;
    memset(&timespec, 0, sizeof(struct timespec));

    /* MUST be realtime: sem_timedwait expects epoch timestamp in abstime*/
    if (clock_gettime(CLOCK_REALTIME, &timespec) == -1){
        if (errnum) *errnum = errno;
        return 1;
    }

    timespec_add_ms__(&timespec, timeout);

    // do not consider timeouts or interrupts erros
    if (sem_timedwait(&esem, &timespec) == -1
            && errno != ETIMEDOUT
            && errno != EINTR
        )
    {
        if (errnum) *errnum = errno;
        return 2;
    }
    return 0;
}

/*
 * Create and initialize event struct for insertion into event queue.
 *
 * <-- return
 *     Dynamically-allocated and initialized `struct event` object.
 *     The object is initialized by having its memory zeroed out
 *     courtesy of calloc().
 *
 * This function always succeeds -- otherwise it exits the program.
 */
static struct event *event_create__(void){
    void *ev = NULL;
    ev = calloc(1, sizeof(struct event));
    //printf("calloc returned ev=%p\n", ev);

    if (!ev){
        say(DEBUG, "Memory allocation failure: failed to mallocate event struct\n");
        exit(MORRE_MEM_ALLOC);
    }

    return (struct event *)ev;
}

/*
 * Destroy (free) a `struct event` object.
 *
 * --> ev @dynamic
 *
 * NOTE: ev->data is lua-managed memory so it cannot and
 * must not be freed.
 */
static inline void event_destroy__(struct event *ev){
    free(ev);
}

/* 
 * Return true if the event is considered to be pending, 
 * else false. 
 *
 * Only interval timer events that do NOT have either MOD
 * or MFD set, and one-off timer events are considered pending.
 * I.e. interval timer events with either MOD or MOD set are
 * not deemed to be pending events.
 */
static inline bool is_pending__(struct event *ev){
    assert(ev);
    if (ev->cyclic && (Mockit_ismfd(ev->mark) || Mockit_hasmod(ev->mark)))
    {
        return false;
    }

    return true;
}

/*
 * Add an event struct to the event queue at the tail end.
 *
 * --> queue
 *     the event queue that EVENT is to be added to.
 *
 * --> event
 *     the event to add to the event queue QUEUE.
 */
static void event_enqueue__(struct event_queue *queue, struct event *event){
    assert(queue && event);

    if (!queue->head){        // list empty : insert new node as both tail and head
        queue->head = event;
        queue->tail = event;
    }
    else{                     // queue not empty; make event the new queue tail
        queue->tail->next = event;
        queue->tail = event;
    }

    queue->count++;
    if (is_pending__(event)) queue->pending++;
}

/*
 * Dequeue event object from the head end of the event queue.
 *
 * Remove an event from the event queue and return it.
 * It's then the responsibility of the caller to call
 * event_destroy() on the value returned by this function when
 * no longer needed.
 *
 * --> queue
 *     the event queue to dequeue an item from (or NULL when empty).
 *
 * <-- return
 *     a dynamically allocated `struct event *` (NULL if the queue
 *     is empty).
 */
static struct event *event_dequeue__(struct event_queue *queue){
    assert(queue);
    struct event *res = NULL;

    if (!queue->head){    // queue empty
        return NULL;
    }

    res = queue->head;    // not empty
    queue->head = res->next;

    if(!queue->head){     // queue empty NOW
        queue->tail = NULL;
    }

    queue->count--;
    if (is_pending__(res)) queue->pending--;

    return res;
}

/*
 * Create an event object and add it to the event queue.
 *  -- thread-safe wrapper around event_enqueue__().
 *
 * @threadsafe
 *
 * <-- return
 *     returns nothing: in case of failure there is no convenient
 *     way to report any errors since this function gets called in a
 *     separate thread.
 */
static void add_event_to_queue__(void *arg){
    struct event *ev = event_create__();

    if (pthread_mutex_lock(&qmtx)) exit(MORRE_MTX);

    ev->data = arg;
    ev->mark = ((struct mockit *)arg)->mark__;
    ev->cyclic = ((struct mockit *)arg)->is_cyclic__ ? true : false;
    struct mockit *mm = ev->data;
    (void) mm;
    say(DEBUG, "-- CALLBACK: add_event_to_queue__: enqueueing event %p, cyclic=%i, mark=%u, mit=%p, ival=%u \n", (void *)ev, ev->cyclic, ev->mark, (void*)ev->data, mm->timeout__);

    //fprintf(stderr, "ev->mark is %i when enqueing\n", ev->mark);

    event_enqueue__(&equeue, ev);

    int errnum = signal_event__();
    if (errnum){
        say(DEBUG, "%s ('%s')\n", "failed to post event", strerror(errnum));
    }

    if(pthread_mutex_unlock(&qmtx)) exit(MORRE_MTX);
}

/*
 * Remove and return event object from the event queue.
 * -- thread-safe wrapper around event_dequeue__().
 *
 * @threadsafe
 *
 * <-- return
 *     a dynamically allocated `struct event *` (or NULL if
 *     the quueue is empty). It's the responsibility of the caller
 *     to call event_destroy() on this when no longer needed so as
 *     to free the associated memory.
 */
static struct event *get_event_from_queue__(void){
    struct event *ev = NULL;

    if (pthread_mutex_lock(&qmtx)) exit(MORRE_MTX);
    ev = event_dequeue__(&equeue);
    if (pthread_mutex_unlock(&qmtx)) exit(MORRE_MTX);

    return ev;
}

/* true if e == q.head, else false */
static inline bool is_qhead(struct event_queue *q, struct event *e){
    return q->head == e ? true : false;
}

/* true if e == q.tail, else false */
static inline bool is_qtail(struct event_queue *q, struct event *e){
    return q->tail == e ? true : false;
}

/*
 * Event queue processor - handle each event in the event queue.
 *
 * This is the function that actually processes the 'events'
 * in the event queue. This is meant to be called from within Lua.
 * The 'events' here are in essence just timer expirations, either
 * one-off or periodic. Each event object is a wrapper for some
 * data, including a reference into the lua registry to a lua callback
 * function that must be called when the event is dequeued/ 'handled'.
 * The callback IS really the whole point of the 'event' in this library.
 * NOTE: the Lua callback function must take no params and return no values.
 *
 * The way this gets called in Lua is typically after the `esem` semaphore
 * is posted in this library, which in turn unblocks the blocking call made
 * from within Lua to wait for events (`luamockit_wait()`).
 *
 * <-- return @Lua
 *     An error is thrown in Lua in case of failure;
 *     the number of events handled is returned otherwise.
 */
int lua_process_events(lua_State *L){
    struct event *event = NULL;
    struct mockit *mit = NULL;
    lua_Integer num_handled = 0;

    // lua callback must take no arguments : discard everything
    lua_settop(L,0);

    while ((event = get_event_from_queue__())){
        //fprintf(stderr, "event->mark is %i\n", event->mark);
        mit = event->data;
        struct luamockit *lmit = mit->ctx;
        lua_State *Lstate = lmit->lua_state;
        say(DEBUG, "handling event=%p mockit=%p with lmit=%p, mark=%u, cyclic=%i, ival=%u\n", (void *)event, (void *)mit, (void *)lmit, event->mark, event->cyclic, mit->timeout__);
 
        lua_rawgeti(Lstate, LUA_REGISTRYINDEX, lmit->lua_cb_ref);
        // printf("cb ref is %i, lmit = %p\n", lmit->lua_cb_ref, (void *)lmit);
        if (!lua_isfunction(Lstate, 1)){
            //luaL_error(Lstate, "failed to retrieve callback from Lua registry");
        }

        /* one-off timers generate a single event => handle then release resources */
        if (!event->cyclic){
            say(DEBUG, "--> one-shot: freeing data\n");

            // lua callback function must take no params and return no values
            lua_pcall(Lstate, 0, 0, 0);
            remove_lua_refs(lmit, false);

            free(lmit);
            free(mit);
        }

        else if(event->cyclic){
            if (!Mockit_ismfd(event->mark) && !Mockit_hasmod(event->mark)){
                lua_pcall(Lstate, 0, 0, 0);
            }

            /* interval timer MOCKIT_MFD-marked event: skip it; this precedes
             * a last MOCKIT_MOD-marked interval timer event */
            else if ( Mockit_ismfd(event->mark) && !Mockit_hasmod(event->mark)){
                say(DEBUG, "mark (%i) has mfd, skipping\n", event->mark);
                event_destroy__(event);
                continue;  /* do not count as 'handled' */
            }
       
            else if (Mockit_hasmod(event->mark)){
                // interval timers are lua-managed userdata: cannot call free on them.
                // one-off timers otoh must be deallocated here.
                say(DEBUG, "@@ HAS MOD MARK SET, CLEANING UP\n");
                remove_lua_refs(lmit, true);
                free(lmit);
                event_destroy__(event);
                continue; /* do not count as 'handled' */
            }
        }

        event_destroy__(event);

        ++num_handled;
    }

    lua_pushinteger(L, num_handled);
    return 1;
}

/*
 * Return the count of pending events in the event queue.
 * 
 * Note this includes all one-off timer events and only interval
 * timer events that have neither MFD nor MOD set because such
 * events are simply dequeued and ignored and not 'handled' (lua
 * callback does not get called back for them).
 * Only one-off timer events and unmarked timer events as described
 * are 'handled' and can be said to be 'pending'.
 *
 * <-- int @lua
 *     count of events in the event queue waiting to be handled.
 */
int get_pending_events_count(lua_State *L){
    lua_settop(L, 0); // no args
    
    /* race condition, but it's inconsequential. We're interested 
     * in the number of events in equeue at the time of checking. */
    lua_pushinteger(L, equeue.pending);

    return 1;
}

/*
 * Return the total count of entries in the event queue.
 
 * Note this includes both pending items (see `get_pending_events_count`
 * above) as well as event structs currently in the queue that will 
 * NOT be handled (specifically interval timer events that have 
 * either MFD or MOD set).
 *
 * <-- int @lua
 *     count of entries currently in the event queue
 */
int get_total_entries_in_queue(lua_State *L){
    lua_settop(L, 0); // no args
    
    /* race condition, but it's inconsequential. We're interested 
     * in the number of events in equeue at the time of checking. */
    lua_pushinteger(L, equeue.count);

    return 1;
}



/*
 * Block until an event is posted to the event queue.
 *
 * The blocking call is achieved by WAITING on the `esem`
 * semaphore, which is incremented with every new event
 * generated. If there are already events in the queue,
 * this call returns immediately.
 *
 * --> timeout
 *     an optional timeout value in milliseconds to block for. After TIMEOUT
 *     milliseconds this function will return regardless of whether any new
 *     events have been added to the event queue or not.
 *
 * <-- return @lua
 *     1 on failure (e.g. sem_wait() failed), 0 on success.
 *     If failed, a second value is returned which is the result
 *     of strerror() from any errno value set inside wait_for_event().
 */
int lua_wait_for_events(lua_State *L){
    uint32_t timeout = 0;
    int errnum = 0;

    // the timeout arg from lua is optional. See the comments on
    // wait_for_event__() for the semantics of this argument
    lua_settop(L,1);
    if (lua_type(L, 1) != LUA_TNIL){   // 1 argument, it must be an integer
        luaL_checktype(L,1,LUA_TNUMBER);
        int res = 0;
        timeout = lua_tointegerx(L, 1, &res);
        if(!res) luaL_error(L,"failed to get timeout arg: must be an integer");
    }

    if (equeue.count) goto success; // return immediately if pending events

    if (wait_for_event__(timeout, &errnum)){     // failed
        lua_pushinteger(L, 1);
        lua_pushfstring(L, "%s", strerror(errnum));
        return 2;
    }

success:
    lua_pushinteger(L, 0);
    return 1;
}

/*
 * Create an interval timer that calls callback on interval expiration.
 *
 * This function creates an interval timer that will expire every
 * INTERVAL milliseconds. The first expiration occurs INTERVAL milliseconds
 * from NOW. On each expiration, an 'event' is added to the
 * global event queue (`equeue`) which gets processed (ideally
 * as often as possible to ensure greater time precision) periodically
 * by lua_process_events (called from within lua), which dequeues and
 * handles every event in the queue until it's empty.
 * Each event added represents a callback that needs to be called back,
 * registered from within a lua script by passing it as the 2nd param
 * to this function.
 *
 * The result of this function, in Lua, is a `struct mockit` full userdata.
 * This MUST be assigned to a variable, in that it identifies the
 * associated interval timer created, which needs to be called
 * destroy() on when no longer needed / when the user wants to disable
 * it.
 *
 * Therefore this function must be called like this for instance:
 *    local timer_obj = luamockit.<this_function>(5,mycallback)
 * and then, when timer_obj is no longer needed:
 *    timer_obj = timer_obj:destroy() // see the timer_destroy() function below
 *
 *
 * The lua thread from within which this function got called is saved and the
 * asssociated event (timer expiration) will call back into lua using this same
 * thread (NOTE: lua thread != POSIX thread);
 * The thread/state is anchored in the registry to ensure it doesn't disappear
 * (it does not get garbage-collected).
 * However, since correct usage of this library normally consists of an infinite
 * loop (see "Lua script Design") mostly sleeping and periodically calling
 * lua_process_events() or spending most of its time waiting on semaphore signalling
 * in lua_process_events(), it's NOT expected that multiple Lua threads will be used.
 * Although this library will, as mentioned, use the Lua thread it was called from,
 * correct behavior is not guaranteed if multiple threads are in actuality used.
 *
 * --> interval @lua
 *     interval value in milliseconds to wait before calling the registered callback.
 *
 * --> Lua function @lua
 *     a callback to call on timer expiration. This must take no arguments.
 *
 * <-- return @lua
 *     a `struct mockit` full userdata to be assigned to a variable in Lua.
 *
 * IMPLEMENTATION NOTES
 * ----------------------
 * An interval 'timer' is implemented simply by a thread oscillating between
 * sleeping for the specified interval and calling the callback. This goes
 * on until the timer is disarmed. Each callback invocation will add an 'event'
 * to the global event queue.
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

    // save Lua callback and lua thread in the Lua registry;
    // fetch them when calling callback on timer expiration
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

    // else, we successfully got the references
    struct mockit *mit = lua_newuserdata(L, sizeof(struct mockit));
    memset(mit, 0, sizeof(struct mockit)); /* lua returns dirty memory */

    struct luamockit *lmit = calloc(1,sizeof(struct luamockit));
    if (!mit || !lmit){
        luaL_error(L,"Failed to allocate memory for timer object");
        return 1;
    }

    /* save reference to userdata in Lua registry */
    lua_pushvalue(L, -1);  // duplicate userdata at the top of the stack
    int lua_userdata_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if(lua_userdata_ref == LUA_REFNIL){
        luaL_error(L,"failed to save mockit userdata in the Lua registry");
        return 1;
    }

    /* save unique references in Lua registry for
     * 1) lua callback 2) lua state 3) mockit userdata */
    lmit->lua_cb_ref = lua_callback_ref;
    lmit->lua_state_ref = lua_state_ref;
    lmit->lua_udata_ref = lua_userdata_ref;
    lmit->lua_state = L;

    mit->ctx = lmit;
    mit->timeout__ = (uint32_t) timeout;
    mit->cb = add_event_to_queue__;
    mit->destructor = mockit_cthread_finalizer;
    mit->is_cyclic__ = true;

    printf(":: ARMING interval timer with ival=%u\n", mit->timeout__);
    if(Mockit_arm(mit)){
        luaL_error(L,"failed to set up interval timer");
        return 1;
    }

    res = luaL_getmetatable(L, MOCKIT_MT);
    if (!res){
        luaL_error(L, "failed to find metatable for mockit object");
        return 1;
    }
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
 * Register a one-off callback from a Lua script i.e. the callback
 * registered only gets called back ONCE.
 *
 * --> timeout, @lua
 *     The duration to wait for, in milliseconds, before calling the callback.
 *
 * --> lua function, @lua
 *     a callback lua function that takes no arguments.
 *
 * <-- return, @lua
 *     Calling this function in LUA does NOT return anything; therefore, unlike
 *     the function for creating an interval timer, the return value of this
 *     function (no return value) doesn't need to be assigned to a variable and
 *     so the destroy() metamethod CANNOT be called on it because the user
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

    uint32_t timeout = (uint32_t) lua_tointegerx(L, 1, &res);
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

    struct luamockit *lmit = calloc(1,sizeof(struct luamockit));

    // allocate and initialize a data object that the callback will be called with
    struct mockit *mit = Mockit_dynamic_init(add_event_to_queue__,
                                            timeout,
                                            false,
                                            lmit,
                                            mockit_cthread_finalizer 
                                            );
    if (!mit || !lmit){
        luaL_error(L,"Failed to allocate memory for timer object");
    }
    lmit->lua_state = L;
    lmit->lua_cb_ref = lua_callback_ref;// unique lua registry ref to callback
    lmit->lua_state_ref= lua_state_ref;   // unique lua registry ref to lua state/thread for anchoring

    // sleep happens in separate thread; therefore we can return to lua immediately
    if(Mockit_arm(mit)){
        luaL_error(L,"failed  to register one-off callback");
    }

    return 0;
}

/*
 * Monitor the event queue and do NOT return until an event with
 * the specified struct mockit memory address is seen that has the
 * MOCKIT_MOD mark set. This is in orded to be certain the event has
 * been seen and destroyed in the wake of disarming a timer.
 * TLDR; this function acts as a guarantor of a timer's associated
 * resources actually being released.
 */
static int wait_for_MOD(struct mockit *addr){
    struct event *curr = NULL;
    struct event *prev = curr;
    struct event *tmp = NULL;
    
    for (;;){
        printf("Waiting for MOD: waiting for event semaphore post for %u ms\n", LUAMOCKIT_MOD_WAIT_TIMEOUT);
        assert(!wait_for_event__(LUAMOCKIT_MOD_WAIT_TIMEOUT, NULL));
        assert(!pthread_mutex_lock(&qmtx));

        curr = prev ? prev : equeue.head;

        long unsigned i = 0;
        while (curr){
            printf(" * iteration around the loop: %lu, count=%lu, pending=%lu\n", ++i, equeue.count, equeue.pending);

            if (curr->data != addr){
                printf(" -- looking at %p, different from %p, continuing\n", curr->data, (void *)addr);
                prev = curr;
                curr = curr->next;
                continue;
            }
            
            printf(" -- found %p at i=%lu, mark=%u, timer=%u\n", (void *)addr, i, curr->mark, ((struct mockit *)curr->data)->timeout__);
            uint32_t timeout = ((struct mockit *)curr->data)->timeout__;
            // curr == addr
            tmp = curr;
 
           // remove actual event from queue 
            if (!is_qhead(&equeue, curr) && !is_qtail(&equeue, curr)){
                printf(" -> current node: not head, not tail!\n");
                curr = curr->next;
                prev->next = curr;
            }

            else if (is_qhead(&equeue, curr) && is_qtail(&equeue, curr)){
                printf(" -> current node: head and tail both!\n");
                curr = NULL;
                equeue.head = equeue.tail = curr;  // empty queue
            }

            else if(is_qhead(&equeue, curr)){
                printf(" -> current node: just head\n");
                equeue.head = curr->next;
                curr = curr->next;
            }
            
            else if (is_qtail(&equeue, curr)){
                printf(" -> current node: just tail\n");
                equeue.tail = prev;
                curr = NULL;
                prev->next = curr;
            }

            uint8_t mark = tmp->mark;
            struct mockit *mit = tmp->data;
            struct luamockit *lmit = mit->ctx;
            bool pending = is_pending__(tmp);
            event_destroy__(tmp);
            equeue.count--;
            
            if (pending){
                printf(" ! normal callback -- destroying, decrementing pending\n");
                equeue.pending--;
            }
            else if(Mockit_ismfd(mark) && !Mockit_hasmod(mark)){
                printf(" ! mfd only found, still waiting for MOD\n");
            }
            else if(Mockit_hasmod(mark)){
                printf(" -- @@ SAW MOD, cleanup complete @@ for %p at i=%lu, mark=%u, timer=%u\n", (void *)addr, i, mark, timeout);
                remove_lua_refs(lmit, true);
                free(lmit);
                goto unlock;
            }
       }
    }
unlock:
    assert(!pthread_mutex_unlock(&qmtx));
    return 0;
}




/*
 * Destroy an interval timer object.
 *
 * --> mockit object, @lua
 *     the struct mockit userdata representing the timer object to destroy.
 *
 * <-- return @lua
 *     nil: this should be assigned back to the timer object being destroyed.
 *
 * This is a metamethod to be called in Lua like this:
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
 * userdata / timer object. Therefore the user MUST NOT have multiple
 * variables pointing to the same object. Each call to
 * lua_get_interval_timer() MUST be assigned to a single unique variable
 * in lua that destroy() is then called on and its return value assigned
 * back to. Otherwise the user must make sure all references (Lua variables
 * pointing to said interval timer object / userdata) are set to nil when
 * the interval timer is no longer needed and its garbage collection is
 * desired.
 */
int destroy_interval_timer(lua_State *L){
    bool wait_and_clean = false;
    // check arguments in Lua
    lua_settop(L, 2);
    luaL_checkudata(L, 1, MOCKIT_MT);
    struct mockit *mit = lua_touserdata(L,1);
    if (!mit){  // NOT a userdata, invalid argument
        luaL_error(L, "Invalid function argument: not a userdata (mockit object expected)");
    }

    if (lua_type(L,2) != LUA_TNIL){
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        wait_and_clean = lua_toboolean(L, 2);
    }
    // can't call free on userdata as that's lua-managed memory;
    // simply disarm timer (mark it for deletion) and return nil to Lua
    //Mockit_disarm(mit);

    //printf("lua stack size is %i\n", lua_gettop(L));

    printf(":: DESTROYING interval timer with mit=%p ival=%u and mark=%u\n", (void*)mit, mit->timeout__, mit->mark__);
    if (wait_and_clean){
        printf("waiting for thread to join \n");
        if (Mockit_destroy(mit)){
            luaL_error(L, "Failed to destroy timer object (pthread_join error)");
        }

        printf("waiting for MOD\n");
        wait_for_MOD(mit);
    }
    else{
        Mockit_disarm(mit);
    }
    //printf("lua stack size is %i\n", lua_gettop(L));
    lua_pushnil(L); // the result of this function should be assigned back to the timer object
    return 1;
}

/*
 * Make a BLOCKING call to clock_nanosleep() in the same thread.
 *
 * --> duration @lua
 *     the amount of time to sleep in milliseconds.
 *
 * --> do_restart @lua
 *     True if the sleep should be restarted/resumed after a signal interrupt.
 *     False if on any interruption the call should give up and return the
 *     number of milliseconds left until sleep would have been completed.
 *
 * <-- remaining
 *     time left to sleep in ms. If do_restart is true sleep always completes,
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

    uint32_t sleep_time = (uint32_t) lua_tointegerx(L,1,&res);
    if (!res){
        luaL_error(L, "incorrect time value specified for sleep duration.");
    }

    bool restart_sleep = false;
    if (lua_type(L, 2) != LUA_TNIL){
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        restart_sleep = lua_toboolean(L, 2);
    }

    uint32_t remaining = 0;
    int error_code     = 0;

    if ((error_code = Mockit_bsleep(sleep_time, restart_sleep, &remaining))){
        luaL_error(L, "Call to sleep failed : %s", strerror(error_code));
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
    int rc = 0;

    // failure
    if (( rc = Mockit_gettime(&secs, &ms)) ){
        lua_pushnil(L);
        lua_pushinteger(L, rc);
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
    if (Mockit_mstimestamp(&timestamp)){
        lua_pushnil(L);
    }
    else{
        lua_pushinteger(L, timestamp);
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
    {"pending", get_pending_events_count},
    {"inqueue", get_total_entries_in_queue},
    {NULL, NULL}
};

/* Module metamethods */
const struct luaL_Reg luamockit_metamethods[] = {
    {"destroy", destroy_interval_timer},
    {NULL, NULL}
};

/* Open/initialize module */
int luaopen_luamockit(lua_State *L){
    assert(lua_initialize(NULL) == 0);            // initialize semaphore etc
    luaL_newmetatable(L, MOCKIT_MT);
    lua_pushvalue(L,-1);
    lua_setfield(L,-2, "__index");            // the metatable's __index method should point to the metatable itself
    luaL_setfuncs(L,luamockit_metamethods,0); // populate MOCKIT_MT with the metamethods in mockits_metamethods

    luaL_newlib(L, luamockit);
    return 1;
}
