#!/usr/bin/env lua

package.path = package.path .. ";./aux/?.lua;../aux/?.lua;"
package.cpath = package.cpath .. ";./out/?.so;../out/?.so;"

local luamockit = require("luamockit")
local mutils = require("mutils")


local tests_run = 0
local tests_passed = 0
local global_buffer = {}

-- used to set the precision. For example, if x is the correct
-- value and ERROR_RANGE is 2, any value e [x-2 .. x+2] will
-- be considered valid
local ERROR_RANGE = os.getenv("ERROR_RANGE") or 3

--[[
    Sleep for SECS seconds and MS milliseconds and check if the mockit library 
    reports time measurements consistent with what you get from lua or the shell.

    Note either secs or ms can be 0 (though it makes no sense to have them BOTH as 0).

    The measurements taken and comparisons made are as follows:

    1) before sleeping, the unix timestamp is recorded (in seconds) with
       os.time() and mockit.time().

    2) once sleep is completed, the unix timestamp is again recoreded, the same
       way as above.

    3) additionally, a timestamp in milliseconds is obtained by using the date command
       via a pipe. A corresponding millisecond timestamp is obtained from the values
       reported by mockit.

    The second-precision measurements are compared for strict equality. The millisecond-precision
    measurements otoh are allowed an off-by-one error. That is, if the correct number of ms
    is x, the value reported by mockit() is allowed to be x +/- 1. This is to account for the time
    it takes to open a pipe and invoke date() via the shell and other processing overheads. 
    This works on my machine but is highly dependent on the speed of the machine the tests 
    are being run on. Nevertheless, a reported value of within 1 (or perhaps 2 or 3) milliseconds
    should be considered valid, while a reported value that's clearly off should be obvious
    by itself.
--]]
function test_time_precision(secs, ms)
    local start_secs = os.time()
    local start_mockit = luamockit.time() -- ignore ms

    os.execute(string.format("sleep %s.%s", secs, ms))
    local end_secs = os.time()
    local end_ms_timestamp = mutils.capture("date +%s%3N")
    local end_mockit_secs, end_mockit_ms = luamockit.time()
    local end_mockit_ms_timestamp = string.format("%s", end_mockit_secs*1000 + end_mockit_ms)
    
    if not (start_secs == start_mockit) then
        mutils.reveal("start values different! start_secs=%s, start_mockit=%s", start_secs, start_mockit)
        return false
    end

    if not (end_secs == end_mockit_secs) then
        mutils.reveal("end values different! end_secs=%s, end_mockit_secs=%s", end_secs, end_mockit_secs)
        return false
    end

    if not(tonumber(end_ms_timestamp) >= tonumber(end_mockit_ms_timestamp)-ERROR_RANGE and
            tonumber(end_ms_timestamp) <= tonumber(end_mockit_ms_timestamp+ERROR_RANGE)) 
            then
        mutils.reveal("end ms timestamps differ! end_ms_timestamp=%s, end_mockit_ms_timestamp=%s", end_ms_timestamp, end_mockit_ms_timestamp)
        return false
    end

    return true
end

function test_mstimestamp(secs, ms)
    os.execute(string.format("sleep %s.%s", secs, ms))
    local ms_timestamp = mutils.capture("date +%s%3N")
    local mockit_mstimestamp = luamockit.mstimestamp()

    if not(tonumber(ms_timestamp) >= mockit_mstimestamp-ERROR_RANGE and
            tonumber(ms_timestamp) <= mockit_mstimestamp+ERROR_RANGE) 
            then
        mutils.reveal("millisecond timestamps differ! ms_timestamp=%s, mockit_mstimestamp=%s", ms_timestamp, mockit_mstimestamp)
        return false
    end
    
    return true
end

function test_sleep_precision(ms)
    local start = luamockit.mstimestamp()
    local expected_end = start + ms
    
    luamockit.sleep(ms, true)
    
    local actual_end = luamockit.mstimestamp()

    if not(expected_end >= actual_end-ERROR_RANGE and expected_end <= actual_end+ERROR_RANGE) then
        mutils.reveal("end ms timestamps differ! end_ms_timestamp=%s, end_mockit_ms_timestamp=%s", end_ms_timestamp, end_mockit_ms_timestamp)
        return false
    end

    return true
end



function dummy_callback()
    print("called lua callback")
    global_buffer[1] = luamockit.mstimestamp()
end


function test_one_off_timer(ms, callback)
    local ERROR_RANGE = 30
    local start_timestamp = luamockit.mstimestamp()
    local expected_end = start_timestamp + ms

    luamockit.oneoff(ms, callback)
    luamockit.wait()
    print("pending event count: ", luamockit.pending())
    luamockit.process_events()

    local actual_end = global_buffer[1]
    
    print(string.format("start_timestamp = %s, end_timestamp = %s", start_timestamp, actual_end))
    if not (expected_end <= actual_end+ERROR_RANGE and expected_end >= actual_end-ERROR_RANGE) then
        mutils.reveal("expected_end (%s) != actual_end (%s) (diff = %s)", expected_end, actual_end, expected_end - actual_end)
        return false
    end
    
    return true
end

function test_interval_timer(duration, interval)
    local ERROR_RANGE = 60;
    local timestamps = {}
    local start_timestamp = luamockit.mstimestamp()
    local end_timestamp = start_timestamp + duration

    -- call this function to add its timestamp on every call to the table
    function callback()
        table.insert(timestamps, luamockit.mstimestamp())
    end
    
    local it = luamockit.getit(interval, callback)

    while (luamockit.mstimestamp() < end_timestamp) do
        luamockit.wait() 
        luamockit.process_events()
    end
    
    it = it:destroy(true)
    print("it is, after destroying it, " .. tostring(it))
    
    local expected = start_timestamp
    for _,stamp in ipairs(timestamps) do
        expected = expected + interval
        mutils.reveal(string.format("comparing expected=%s and actual=%s", expected, stamp))
        if not (expected >= stamp-ERROR_RANGE and expected <= stamp+ERROR_RANGE) then
            mutils.reveal("invalid timestamp obtained from interval timer. Expected '%s', got '%s'", expected, stamp)
            return false
        end
    end
    return true
end

function run_test(f, ...)
    local res = f(...)
    tests_run = tests_run+1
    if res then tests_passed = tests_passed+1 end 
    print(string.format(" * test %s %s", tests_run, res and "PASSED" or "FAILED !!! "))
end


print(" ===================== Running luamockit tests ======================= ... ")
print(" @ Running time precision tests on mockit.time()")
run_test(test_time_precision, 1, 230)
run_test(test_time_precision, 0, 130)
run_test(test_time_precision, 0, 2000)
run_test(test_time_precision, 0, 1785)
run_test(test_time_precision, 0, 013)
run_test(test_time_precision, 0, 007)
run_test(test_time_precision, 0, 001)
run_test(test_time_precision, 3, 1111)

print(" @ Validating precision of mockit.mstimestamp()")
run_test(test_mstimestamp, 1, 230)
run_test(test_mstimestamp, 0, 130)
run_test(test_mstimestamp, 0, 2000)
run_test(test_mstimestamp, 0, 1785)
run_test(test_mstimestamp, 0, 013)
run_test(test_mstimestamp, 0, 007)
run_test(test_mstimestamp, 0, 001)
run_test(test_mstimestamp, 3, 1111)
run_test(test_mstimestamp, 3, 613)

print(" @ Running time precision tests on mockit.sleep()")
run_test(test_sleep_precision, 231)
run_test(test_sleep_precision, 30)
run_test(test_sleep_precision, 2001)
run_test(test_sleep_precision, 1787)
run_test(test_sleep_precision, 013)
run_test(test_sleep_precision, 007)
run_test(test_sleep_precision, 001)
run_test(test_sleep_precision, 11111)

print(" @ Testing one-off callbacks -- mockit.oneoff() ...")
run_test(test_one_off_timer, 5555, dummy_callback)
run_test(test_one_off_timer, 3000, dummy_callback)
run_test(test_one_off_timer, 2783, dummy_callback)
run_test(test_one_off_timer, 1, dummy_callback)
run_test(test_one_off_timer, 7, dummy_callback)
run_test(test_one_off_timer, 13, dummy_callback)
run_test(test_one_off_timer, 1300, dummy_callback)

print(" @ Testing precision of interval timers ... ")
run_test(test_interval_timer, 3000, 100)
run_test(test_interval_timer, 400, 1)
run_test(test_interval_timer, 1273, 100)
run_test(test_interval_timer, 2001, 7)
run_test(test_interval_timer, 10000, 2000)
run_test(test_interval_timer, 7000, 70)
run_test(test_interval_timer, 21000, 93)
run_test(test_interval_timer, 77, 7)

print("left == " .. luamockit.pending())


function dummy()
    print("callled dummy")
end

print(string.format("passed: %s of %s", tests_passed, tests_run))
if not (tests_passed == tests_run) then os.exit(11) end

