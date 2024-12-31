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

/** @file
 * @brief Implementation of the utilities for the watchdog application.
 */

// The Linux/Posix stuff.
#include <signal.h>

// Us.
#include <w_util.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Flag that tells us whether or not we've had a CTRL-C.
static volatile sig_atomic_t gKeepGoing = true;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Catch a termination signal.
static void terminateSignalHandler(int signal)
{
    gKeepGoing = false;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Capture CTRL-C so that we can exit in an organised fashion.
void wUtilTerminationCaptureSet()
{
    signal(SIGINT, terminateSignalHandler);
}

// Force termination.
void wUtilTerminationSet()
{
   gKeepGoing = false;
}

// Return whether the program should continue running or not.
bool wUtilKeepGoing()
{
    return gKeepGoing;
}

// Initialise a time-out with the current time.
wUtilTimeoutStart_t wUtilTimeoutStart()
{
    wUtilTimeoutStart_t startTime;
    startTime.time = std::chrono::high_resolution_clock::now();
    return startTime;
}

// Perform a time-out check in a wrap-safe way.
bool wUtilTimeoutExpired(wUtilTimeoutStart_t startTime,
                         std::chrono::nanoseconds duration)
{
    auto nowTime = std::chrono::high_resolution_clock::now();
    auto elapsedTime = nowTime - startTime.time;
    return elapsedTime > duration;
}

// Update a timing monitoring buffer.
void wUtilMonitorTimingUpdate(wUtilMonitorTiming_t *monitorTiming)
{
    std::chrono::time_point<std::chrono::high_resolution_clock> timestamp;
    std::chrono::duration<double> gap = std::chrono::high_resolution_clock::duration::zero();

    // Get the current timestamp, if possible work out the gap
    // from the last and update the largestGap based on that
    timestamp = std::chrono::high_resolution_clock::now();
    if (monitorTiming->numGaps > 0) {
        gap = timestamp - monitorTiming->previousTimestamp;
        if (gap > monitorTiming->largest) {
            monitorTiming->largest = gap;
        }
    }

    // Now deal with the total, and hence the average
    if (monitorTiming->oldestGap == NULL) {
        // Haven't yet filled the monitoring buffer up, just add the
        // new gap and update the total
        monitorTiming->gap[monitorTiming->numGaps] = gap;
        monitorTiming->numGaps++;
        monitorTiming->total += gap;
        if (monitorTiming->numGaps >= W_UTIL_ARRAY_COUNT(monitorTiming->gap)) {
            monitorTiming->oldestGap = &(monitorTiming->gap[0]);
        }
    } else {
        // The monitoring buffer is full, need to rotate it
        monitorTiming->total -= *monitorTiming->oldestGap;
        *monitorTiming->oldestGap = gap;
        monitorTiming->total += gap;
        monitorTiming->oldestGap++;
        if (monitorTiming->oldestGap >= monitorTiming->gap + W_UTIL_ARRAY_COUNT(monitorTiming->gap)) {
            monitorTiming->oldestGap = &(monitorTiming->gap[0]);
        }
    }

    if (monitorTiming->numGaps > 0) {
        // Note: the average becomes an unsigned value unless the
        // denominator is cast to an integer
        monitorTiming->average = monitorTiming->total / (int) monitorTiming->numGaps;
    }

    // Store the timestamp for next time
    monitorTiming->previousTimestamp = timestamp;
}

// End of file
