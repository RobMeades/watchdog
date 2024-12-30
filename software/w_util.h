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

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Structure to hold a start time, used in time-out calculations.
 */
typedef struct {
    std::chrono::time_point<std::chrono::high_resolution_clock> time;
} wUtilTimeoutStart_t;

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

#endif // _W_UTIL_H_

// End of file
