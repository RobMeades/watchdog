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

// The CPP stuff.
#include <thread>

// The Linux/Posix stuff.
#include <signal.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

// Other parts of watchdog.
#include <w_log.h>

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

// Stop a thread and timer.
static void cleanUpThreadTimed(int *timerFd,
                               std::thread *thread = nullptr,
                               bool *keepGoingFlag = nullptr)
{
    if ((timerFd != nullptr) && (*timerFd >= 0)) {
        if (thread != nullptr) {
            if (keepGoingFlag) {
                *keepGoingFlag = false;
            }
            if (thread->joinable()) {
               thread->join();
            }
        }
        close(*timerFd);
        *timerFd = -1;
    }
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

// Create and start a real-time thread driven by a tick-timer.
int wUtilThreadTickedStart(wCommonThreadPriority_t priority,
                           int periodMs,
                           bool *keepGoingFlag,
                           wUtilThreadFunction_t *loop,
                           const char *name,
                           std::thread *thread,
                           void *context)
{
    int timerFdOrErrorCode = -EINVAL;

    if ((priority <= 0) && (periodMs > 0) && keepGoingFlag && loop && thread) {
        // Create the timer
        timerFdOrErrorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (timerFdOrErrorCode >= 0) {
            struct itimerspec timerSpec = {};
            timerSpec.it_value.tv_sec = periodMs / 1000;
            timerSpec.it_value.tv_nsec = (periodMs % 1000) * 1000000;
            timerSpec.it_interval = timerSpec.it_value;
            if (timerfd_settime(timerFdOrErrorCode, 0, &timerSpec, nullptr) < 0) {
                timerFdOrErrorCode = -errno;
                W_LOG_ERROR("unable to set thread timer, error code %d.",
                            timerFdOrErrorCode);
            }
        } else {
            timerFdOrErrorCode = -errno;
            W_LOG_ERROR("unable to create thread timer, error code %d.",
                        timerFdOrErrorCode);
        }
        if (timerFdOrErrorCode >= 0) {
            // Create the thread
            try {
                // This will go bang if the thread cannot be created
                *keepGoingFlag = true;
                *thread = std::thread(loop,
                                      timerFdOrErrorCode,
                                      keepGoingFlag, context);
            }
            catch (int x) {
                // Tidy up on error
                cleanUpThreadTimed(&timerFdOrErrorCode);
                timerFdOrErrorCode = -x;
                W_LOG_ERROR("unable to start or set schedule of thread,"
                            " error code %d.", timerFdOrErrorCode);
            }
        }
        if (timerFdOrErrorCode >= 0) {
            // Set the required priority
            struct sched_param scheduling;
            scheduling.sched_priority = W_COMMON_THREAD_REAL_TIME_PRIORITY(priority);
            if (pthread_setschedparam(thread->native_handle(),
                                      SCHED_FIFO, &scheduling) != 0) {
                cleanUpThreadTimed(&timerFdOrErrorCode, thread, keepGoingFlag);
                timerFdOrErrorCode = -errno;
            }
        }
        if (timerFdOrErrorCode >= 0) {
            // Set the thread name
            if (name && (pthread_setname_np(thread->native_handle(), name) < 0)) {
                cleanUpThreadTimed(&timerFdOrErrorCode, thread, keepGoingFlag);
                timerFdOrErrorCode = -errno;
            }
        }
    }

    return timerFdOrErrorCode;
}

// Stop a thread and timer.
void wUtilThreadTickedStop(int *timerFd, std::thread *thread, bool *keepGoingFlag)
{
    cleanUpThreadTimed(timerFd, thread, keepGoingFlag);
}

// Poll the given timer for expiry.
int wUtilBlockTimer(int timerFd, int guardMs)
{
    int numExpiriesOrErrorCode = -EINVAL;

    if (timerFd >= 0) {
        numExpiriesOrErrorCode = 0;
        uint64_t numExpiries = 0;
        struct pollfd pollFd[1] = {};
        struct timespec timeSpec = {};
        timeSpec.tv_sec = guardMs / 1000;
        timeSpec.tv_nsec = (guardMs % 1000) * 1000000;
        sigset_t sigMask;

        pollFd[0].fd = timerFd;
        pollFd[0].events = POLLIN;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);

        int numEvents = ppoll(pollFd, 1, &timeSpec, &sigMask);
        if (numEvents > 0) {
            if (pollFd[0].revents & POLLIN) {
                numExpiriesOrErrorCode = read(timerFd, &numExpiries,
                                              sizeof(numExpiries));
                if (numExpiriesOrErrorCode == sizeof(numExpiries)) {
                    numExpiriesOrErrorCode = (int) numExpiries;
                } else {
                    numExpiriesOrErrorCode = -errno;
                }
            }
        } else if (numEvents < 0) {
            numExpiriesOrErrorCode = -errno;
        }
        // Don't need to worry about numEvents = 0 as numExpiriesOrErrorCode
        // will alredy be zero here
    }

    return numExpiriesOrErrorCode;
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

// Return the directory part of a file path.
std::string wUtilDirectoryPathGet(std::string path, bool absolute)
{
    if (!path.empty()) {
        if (absolute && !(path.find_first_of(W_UTIL_DIR_SEPARATOR) == 0)) {
            // If we haven't already got an absolute path, make it absolute
            char *currentDirName = get_current_dir_name();
            if (currentDirName) {
                path = std::string(currentDirName) + W_UTIL_DIR_SEPARATOR + path;
                free(currentDirName);
            }
        }
        // Find the last slash and cut there
        unsigned int lastDirSeparatorPos = path.find_last_of(W_UTIL_DIR_SEPARATOR);
        if (lastDirSeparatorPos != std::string::npos) {
            path = path.substr(0, lastDirSeparatorPos);
        } else {
            // If there are no slashes, the directory must be the
            // current one
            path = std::string(W_UTIL_DIR_THIS);
        }
    }

    return path;
}

// End of file
