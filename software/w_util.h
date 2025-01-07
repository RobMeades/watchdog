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

// This API is dependent on std::chrono.
#include <chrono>

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

#ifndef W_UTIL_MONITOR_TIMING_LENGTH
/** The number of ticks to average timing over when monitoring.
 */
# define W_UTIL_MONITOR_TIMING_LENGTH 1000
#endif

/** Return the absolute value of a signed integer
 */
# define W_UTIL_ABS(x) ((x) >= 0 ? (x) : -(x))

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Structure to hold a start time, used in time-out calculations.
 */
typedef struct {
    std::chrono::time_point<std::chrono::high_resolution_clock> time;
} wUtilTimeoutStart_t;

/** Structure to monitor timing; should be provided initialised to
 * zero and then wUtilMonitorTimingUpdate() should be called to
 * update it at every tick.  The interesting fields is then likely
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

/** Return the program shoudl continue running or not.
 *
 * @return true if the program should continue running, else false.
 */
bool wUtilKeepGoing();

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

#endif // _W_UTIL_H_

// End of file
