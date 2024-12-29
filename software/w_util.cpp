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

// End of file
