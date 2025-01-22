/*
 * Copyright 2025 Rob Meades
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _W_UTIL_H_
#define _W_UTIL_H_

// This API is dependent on std::string, std::chrono std::thread and,
// for wCommonThreadPriority_t, w_common.h
#include <string>
#include <chrono>
#include <thread>
#include <w_common.h>

/** @file
 * @brief The utilities API for the watchdog application; this API is
 * thread-safe.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/** Compute the number of elements in an array.
 */
#define W_UTIL_ARRAY_COUNT(array) (sizeof(array) / sizeof(array[0]))

/** Used only by W_UTIL_STRINGIFY_QUOTED.
 */
#define W_UTIL_STRINGIFY_LITERAL(x) #x

/** Stringify a macro, so if you have:
 *
 * #define foo bar
 *
 * ...W_UTIL_STRINGIFY_QUOTED(foo) is "bar".
 */
#define W_UTIL_STRINGIFY_QUOTED(x) W_UTIL_STRINGIFY_LITERAL(x)

/** The directory separator (we only run this on Linux).
 */
#define W_UTIL_DIR_SEPARATOR "/"

/** The character that means "this directory".
 */
#define W_UTIL_DIR_THIS "."

/** The required appendage to a system command to make it silent
 * (on Linux, obviously).
 */
#define W_UTIL_SYSTEM_SILENT " >>/dev/null 2>>/dev/null"

#ifndef W_UTIL_POLL_TIMER_GUARD_MS
/** The default poll guard timer in milliseconds, see wUtilBlockTimer().
 */
# define W_UTIL_POLL_TIMER_GUARD_MS 1000
#endif

#ifndef W_UTIL_MONITOR_TIMING_LENGTH
/** The number of ticks to average timing over when monitoring.
 */
# define W_UTIL_MONITOR_TIMING_LENGTH 1000
#endif

/** Return the absolute value of a signed integer.
 */
# define W_UTIL_ABS(x) ((x) >= 0 ? (x) : -(x))

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Function signature for the thread function passed to
 * wUtilThreadTickedStart().
 *
 * @param timerFd   the file descriptor of the timer that is to drive
 *                  the thread.
 * @param keepGoing a pointer to a flag which will be true when
 *                  the function is called and which the function
 *                  should monitor: if the flag is ever set to
 *                  false the function should exit; will never be
 *                  nullptr.
 * @param context   the context pointer that was passed to 
 *                  wUtilThreadTickedStart() when the thread
 *                  was created.
 */
typedef void (wUtilThreadFunction_t)(int timerFd, bool *keepGoing,
                                     void *context);

/** Structure to hold a start time, used in time-out calculations.
 */
typedef struct {
    std::chrono::time_point<std::chrono::high_resolution_clock> time;
} wUtilTimeoutStart_t;

/** Structure to monitor timing; should be provided initialised to
 * zero and then wUtilMonitorTimingUpdate() should be called to
 * update it at every tick.  The interesting field is then likely
 * the "largest" gap field, being the largest gap between updates.
 */
typedef struct {
    std::chrono::time_point<std::chrono::high_resolution_clock> previousTimestamp;
    std::chrono::duration<double> gap[W_UTIL_MONITOR_TIMING_LENGTH];
    unsigned int numGaps;
    // This is non-NULL only when duration has W_MONITOR_TIMING_LENGTH entries
    std::chrono::duration<double> *oldestGap;
    std::chrono::duration<double> total;
    std::chrono::duration<double> largest;
    std::chrono::duration<double> average;
} wUtilMonitorTiming_t;

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Set capture of program termination, i.e. CTRL-C.
 */
void wUtilTerminationCaptureSet();

/** Set the termination flag "manually".
 */
void wUtilTerminationSet();

/** Return whether the program should continue running or not.
 *
 * @return true if the program should continue running, else false.
 */
bool wUtilKeepGoing();

/** Create and start a thread with the given priority, driven by
 * a tick-timer of the given period.
 *
 * @param priority      the thread priority.
 * @param periodMs      the tick-timer period in milliseconds.
 * @param keepGoingFlag a pointer to a flag that will be set to
 *                      true if this function returns successfully
 *                      and may be set to false to cause the loop
 *                      function to exit; cannot be nullptr.  This
 *                      pointer will be passed to the loop function
 *                      as its second parameter, so you probably
 *                      shouldn't point it at a stack variable.
 *                      loop() should return if keepGoingFlag is
 *                      set to false.
 * @param loop          the loop function that implements the thread;
 *                      cannot be nullptr.
 * @param name          a name for the thread; may be nullptr.  The
 *                      length of the name may be limited by Linux
 *                      pthread_setname_np(), e.g. to 16 characters;
 *                      an error will be returned if the name is too
 *                      long.
 * @param thread        a place to put the thread; cannot be nullptr.
 * @param context       a pointer to a user context that will be passed
 *                      to loop() as its last parameter.
 * @return              on success the file descriptor of the timer
 *                      driving the thread, else negative error code;
 *                      when loop() is called this file descriptor
 *                      will be passed to it as its first parameter.
 */
int wUtilThreadTickedStart(wCommonThreadPriority_t priority,
                           int periodMs,
                           bool *keepGoingFlag,
                           wUtilThreadFunction_t *loop, 
                           const char *name,
                           std::thread *thread,
                           void *context = nullptr);

/** Poll the given timer for expiry, returning when the timer
 * has expired at least once or if CTRL-C is pressed or if
 * the guard timer is hit.  A ticked thread created by a call
 * to wUtilThreadTickedStart() may call this function to determine
 * if its timer has expired and then perform some action.
 *
 * @param timerFd the file descriptor of the timer to poll, must
 *                be a non-negative number.
 * @param guardMs the guard time in milliseconds.
 * @return        the number of timer expiries that have occurred
 *                since the last call to this function, else
 *                negative error code; 0 means that the guard
 *                timer has been hit or possibly CTRL-C has been
 *                detected.
 */
int wUtilBlockTimer(int timerFd,
                    int guardMs = W_UTIL_POLL_TIMER_GUARD_MS);

/** Stop a thread and tick-timer that were created with
 * wUtilThreadTickedStart().  When this function has returned
 * the timer and thread no longer exist, wUtilThreadTickedStart()
 * should be called to create them again.
 *
 * @param timerFd        a pointer to the file descriptor of the
 *                       tick-timer that was orginally returned by
 *                       wUtilThreadTickedStart().  On return,
 *                       *timerFd will be set to -1.  If nullptr or
 *                       a pointer to a negative value this function
 *                       will do nothing.
 * @param thread         a pointer to the thread that was populated by
 *                       wUtilThreadTickedStart(); ignored if timerFd
 *                       is nullptr or a pointer to a negative value.
 * @param keepGoingFlag  a pointer to the keep-going flag that was
 *                       passed to wUtilThreadTickedStart(); ignored
 *                       if timerFd is nullptr or a pointer to a
 *                       negative value. If nullptr then thread must
 *                       have already exited before this function
 *                       is called, else this function will block
 *                       until thread does exit.  If not nullptr,
 *                       *keepGoingFlag will be set to false when this
 *                       function returns.
 */
void wUtilThreadTickedStop(int *timerFd, std::thread *thread,
                           bool *keepGoingFlag);

/** Initialise a time-out with the current time.
 *
 * @return a timeout structure populated with the current time.
 */
wUtilTimeoutStart_t wUtilTimeoutStart();

/** Perform a time-out check in a wrap-safe way.
 *
 * @param startTime the start time, as returned by wUtilTimeoutStart().
 * @param duration  the time-out to check against.
 * @return          true if duration has elapsed since startTime,
 *                  else false,
 */
bool wUtilTimeoutExpired(wUtilTimeoutStart_t startTime,
                         std::chrono::nanoseconds duration);

/** Update a timing monitoring buffer; see the definition of
 * wUtilMonitorTiming_t for more information.
 *
 * @param monitorTiming a poinner to the monitoring buffer.
 */
void wUtilMonitorTimingUpdate(wUtilMonitorTiming_t *monitorTiming);

/** Given a string that is assumed to be a path, return the directory
 * portion of that.
 *
 * @param path     a file path.
 * @param absolute whether the returned path should be absolute or not.
 * @return         the directory part of path.
 */
std::string wUtilDirectoryPathGet(std::string path, bool absolute = false);

#endif // _W_UTIL_H_

// End of file
