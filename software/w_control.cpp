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
 * @brief The implementation of the control API for the watchdog
 * application.
 */

// The CPP stuff.
#include <thread>
#include <mutex>
#include <atomic>
#include <list>

// The Linux/Posix stuff.
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>

// The OpenCV stuff (for cv::Point).
#include <opencv2/core/types.hpp>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>
#include <w_msg.h>
#include <w_image_processing.h>
#include <w_video_encode.h>
#include <w_led.h>
#include <w_motor.h>

// Us.
#include <w_control.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Storage for our pointView.
 */
typedef struct {
    cv::Point pointView[W_CONTROL_FOCUS_AVERAGE_LENGTH];
    unsigned int number;
    // This is non-NULL only when pointView[] is full
    cv::Point *oldestPointView;
    cv::Point totalPointView;
    cv::Point averagePointView;
} wControlPointView_t;

/** Storage for a sequence of steps.
 */
typedef struct {
    int stepUnit; // +1 for positive, -1 for negative
    std::list<int> durationTicksList; // Duration is signed as we use negative values in a count-down
} wControlSteps_t;

/** Context for control message handlers.
 */
typedef struct {
    int fd;
    std::thread thread;
    std::mutex mutex;
    std::atomic<bool> staticCamera;
    std::atomic<bool> moving;
    std::atomic<int> intervalCountTicks;
    wControlPointView_t focus;
} wControlContext_t;

/** Control message types.
 */
typedef enum {
    W_CONTROL_MSG_TYPE_FOCUS_CHANGE    // wControlMsgBodyFocusChange_t
} wControlMsgType_t;

/** The message body structure corresponding to our one message:
 * W_CONTROL_MSG_TYPE_FOCUS_CHANGE.
 */
typedef struct {
    cv::Point pointView;
    int areaPixels;
} wControlMsgBodyFocusChange_t;

/** Union of message bodies; if you add a member here you must add
* a type for it in wControlMsgType_t.
 */
typedef union {
    wControlMsgBodyFocusChange_t focusChange;   // W_CONTROL_MSG_TYPE_FOCUS_CHANGE
} wControlMsgBody_t;

/** A structure containing the message handling function
 * and the message type it handles, for use in gMsgHandler[].
 */
typedef struct {
    wControlMsgType_t msgType;
    wMsgHandlerFunction_t *function;
} wControlMsgHandler_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// NOTE: there are more messaging-related variables below
// the definition of the message handling functions.

// A local keep-going flag.
static bool gKeepGoing = false;

// The ID of the control message queue.
static int gMsgQueueId = -1;

// Timer that is employed by controlLoop().
static int gTimerFd = -1;

// Context for control stuff, used by the control loop and passed
// to the message handlers.
static wControlContext_t gContext = {};

// NOTE: there are more messaging-related variables below
// the definition of the message handling functions.

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MISC
 * -------------------------------------------------------------- */

// Handle a focus change: conforms to the function signature of
// wImageProcessingFocusFunction_t and just puts the focus change
// on our queue.
static int focusCallback(cv::Point pointView, int areaPixels)
{
    int queueLengthOrErrorCode = -EBADF;

    if (gMsgQueueId >= 0) {
        wControlMsgBodyFocusChange_t msg = {.pointView = pointView,
                                            .areaPixels = areaPixels};
        queueLengthOrErrorCode = wMsgPush(gMsgQueueId,
                                          W_CONTROL_MSG_TYPE_FOCUS_CHANGE,
                                          &msg, sizeof(msg));
    }

    return queueLengthOrErrorCode;
}

// Work out the distance squared to a point x, y from the origin.
static int distanceSquared(const cv::Point *point)
{
    return (point->x * point->x) + (point->y * point->y);
}

// Convert a time in milliseconds to the number of ticks of the
// control loop.
static int64_t msToTicks(int64_t milliseconds)
{
    return milliseconds / W_CONTROL_TICK_TIMER_PERIOD_MS;
}

// Convert a time in ticks of the control loop into milliseconds;
// this is only used for debug prints.
static int ticksToMs(int ticks)
{
    return ticks * W_CONTROL_TICK_TIMER_PERIOD_MS;
}

// Calibrate a motor and return it to the rest position; used by
// timedMotorMove() for periodic re-calibration and
// step() if stepping results in the need for calibration.
static int motorCalibrateAndMoveToRest(wMotorType_t type,
                                       unsigned int *calibrationAttemptFailureCount)
{
    int errorCode = wMotorCalibrate(type);

    if ((errorCode != 0) && calibrationAttemptFailureCount) {
        (*calibrationAttemptFailureCount)++;
    }

    // Move to the rest position on a best-effort basis
    wMotorMoveToRest(type);

    return errorCode;
}

// Return a motor to the rest position; used by
// timedMotorMove() for periodic return to rest.
static int motorMoveToRest(wMotorType_t type,
                           unsigned int *calibrationAttemptFailureCount)
{
    (void) calibrationAttemptFailureCount;
    return wMotorMoveToRest(type);
}

// Ensure calibration of all motors
static bool motorEnsureCalibration(unsigned int *calibrationAttemptFailureCount)
{
    bool aMotorWasCalibrated = false;

    for (unsigned int m = 0; m < W_MOTOR_TYPE_MAX_NUM; m++) {
        if (wMotorNeedsCalibration((wMotorType_t) m) &&
            (motorCalibrateAndMoveToRest((wMotorType_t) m,
                                         calibrationAttemptFailureCount) == 0)) {
            aMotorWasCalibrated = true;
        }
    }

    return aMotorWasCalibrated;
}

// Get the steps per pixel ratio for the given motor.
// The only reason this would fail is if the motor
// in question is not calibrated, hence an appropriate
// recovery action would be to check for that.
static int stepsPerPixelX100Get(wMotorType_t type)
{
    int stepsPerPixelX100OrErrorCode;

    // Get the range of the motor and work out how
    // many steps it takes to move one pixel, stored
    // multiplied by 100 so that we can do integer
    // arithmetic without losing precision
    stepsPerPixelX100OrErrorCode = wMotorRangeGet(type);
    if (stepsPerPixelX100OrErrorCode >= 0) {
        if (type == W_MOTOR_TYPE_VERTICAL) {
            stepsPerPixelX100OrErrorCode = (stepsPerPixelX100OrErrorCode * 100) /
                                           W_COMMON_HEIGHT_PIXELS;
        } else if (type == W_MOTOR_TYPE_ROTATE) {
            stepsPerPixelX100OrErrorCode = (stepsPerPixelX100OrErrorCode * 100) /
                                            W_COMMON_WIDTH_PIXELS;
        } else {
            stepsPerPixelX100OrErrorCode = -EINVAL;
        }
    }

    return stepsPerPixelX100OrErrorCode; 
}

// Set the steps per pixel ratio for the motors; 
// stepsPerPixelX100 must point to an array of size
// W_MOTOR_TYPE_MAX_NUM.
// Note: the only reason this would fail is if a motor
// is not calibrated, hence a good recovery action would
// be to check for that
static int stepsPerPixelX100Set(unsigned int *stepsPerPixelX100)
{
    int errorCode = 0;

    for (unsigned int m = 0; m < W_MOTOR_TYPE_MAX_NUM; m++) {
        int x = stepsPerPixelX100Get((wMotorType_t) m);
        if (x > 0) {
            if (stepsPerPixelX100) {
                *(stepsPerPixelX100 + m) = x;
            }
        } else {
            W_LOG_WARN("unable to get range for motor %s (%d)!",
                      (wMotorNameGet((wMotorType_t) m)).c_str(), x);
            if (errorCode == 0) {
                errorCode = x;
            }
        }
    }

    return errorCode;
}

// Create an array of steps; stepsPerPixelX100 and steps must be
// pointers to arrays of size W_MOTOR_TYPE_MAX_NUM.
static bool stepListSet(const cv::Point *focus,
                        const unsigned int *stepsPerPixelX100,
                        wControlSteps_t *steps)
{
    bool stepping = false;

    if (focus && stepsPerPixelX100 && steps) {
        // The centre of our point of view is 0, 0, with +Y upwards,
        // -Y downwards, +X to the right, -X to the left, like a
        // conventional X/Y graph.
        //
        // The motion of the motors is similarly positive on
        // W_MOTOR_TYPE_VERTICAL to move upwards, negative to move
        // downwards, positive on W_MOTOR_TYPE_ROTATE to rotate to the
        // right, negative to rotate to the left.
        //
        // When a new focus point arrives it will update the average;
        // here we figure out if the focus has moved sufficiently
        // to warrant movement and then make that move, trying to
        // put our 0, 0 point at the new focus.
        if (distanceSquared(focus) > W_COMMON_FOCUS_CHANGE_THRESHOLD_PIXELS *
                                     W_COMMON_FOCUS_CHANGE_THRESHOLD_PIXELS) {
            W_LOG_DEBUG_START("focus %d, %d is more than %d pixels from the"
                              " origin so create a step list:",
                              focus->x, focus->y,
                              W_COMMON_FOCUS_CHANGE_THRESHOLD_PIXELS);
            // Work out what the distance in pixels means in terms of
            // steps of the rotate and vertical motors
            // These two arrays MUST have the same dimension and
            // it must be W_MOTOR_TYPE_MAX_NUM
            int stepUnit[W_MOTOR_TYPE_MAX_NUM];
            unsigned int numberOfSteps[W_MOTOR_TYPE_MAX_NUM];
            for (unsigned int m = 0; m < W_UTIL_ARRAY_COUNT(numberOfSteps); m++) {
                int coordinate = 0;
                if ((wMotorType_t) m == W_MOTOR_TYPE_VERTICAL) {
                    coordinate = focus->y;
                } else if ((wMotorType_t) m == W_MOTOR_TYPE_ROTATE) {
                    coordinate = focus->x;
                }
                // This just to avoid two stars in the same line of maths just below
                int multiplier = *(stepsPerPixelX100 + m);
                int s = (coordinate * multiplier) / 100;
                stepUnit[m] = 1;
                if (s < 0) {
                    stepUnit[m] = -1;
                }
                numberOfSteps[m] = s * stepUnit[m];
                if (m == 0) {
                    W_LOG_DEBUG_MORE(" %+d vertical", s);
                } else {
                    W_LOG_DEBUG_MORE(", %+d rotate", s);
                }
            }

            // Assemble the step list for each motor
            for (unsigned int m = 0; m < W_UTIL_ARRAY_COUNT(stepUnit); m++) {
                // Set the direction for all steps
                (steps + m)->stepUnit = stepUnit[m];
                // For each step, create a duration, ramping up at the
                // start and down at the end
                unsigned int rampUpDownSteps = ((numberOfSteps[m] * W_CONTROL_MOVE_RAMP_PERCENT) / 100) >> 1;
                unsigned int durationAdderTicks = msToTicks(W_CONTROL_STEP_INTERVAL_MAX_MS) / rampUpDownSteps;
                unsigned int rampUpStopStep = rampUpDownSteps;
                unsigned int rampDownStartStep = numberOfSteps[m] - rampUpDownSteps;
                // Clear the list
                (steps + m)->durationTicksList.clear();
                for (unsigned int s = 0; s < numberOfSteps[m]; s++) {
                    // Work out the duration of this step
                    int durationTicks = 1;
                    if (s < rampUpStopStep) {
                        durationTicks += durationAdderTicks * (rampUpStopStep - s);
                    } else if (s > rampDownStartStep) {
                        durationTicks += durationAdderTicks * (s - rampDownStartStep);
                    }
                    // Add the new step to the list
                    (steps + m)->durationTicksList.push_back(durationTicks);
                    stepping = true;
                }
                if (m == 0) {
                    W_LOG_DEBUG_MORE(", ramp/mid/ramp");
                } else {
                    W_LOG_DEBUG_MORE(" vertical,");
                }
                W_LOG_DEBUG_MORE(" %+d/%+d/%+d", rampUpStopStep * stepUnit[m],
                                 (rampDownStartStep - rampUpStopStep) * stepUnit[m],
                                 (numberOfSteps[m] - rampDownStartStep) * stepUnit[m]);
                if (m == 1) {
                    W_LOG_DEBUG_MORE(" rotate");
                }
            }

            W_LOG_DEBUG_MORE(".");
            W_LOG_DEBUG_END;
        }
    }

    return stepping;
}

// Perform a step: steps must be a pointer to an array
// of size W_MOTOR_TYPE_MAX_NUM.
static bool step(wControlSteps_t *steps, bool staticCamera,
                 std::atomic<int> *intervalCountTicks,
                 int *motorRecalibrateCountTicks,
                 unsigned int *calibrationAttemptFailureCount)
{
    bool movingAtStart = false;
    bool movingAtEnd = false;

    if (steps) {
        for (unsigned int m = 0; m < W_MOTOR_TYPE_MAX_NUM; m++) {
            wControlSteps_t *item = steps + m;
            if (!item->durationTicksList.empty()) {
                movingAtStart = true;
                // Get the duration of the next step
                int durationTicks = item->durationTicksList.front();
                item->durationTicksList.pop_front();
                // If the duration is positive, perform the step
                if (durationTicks > 0) {
                    bool motorNeedsCalibration = false;
                    if (!staticCamera) {
                        wMotorMove((wMotorType_t) m, item->stepUnit);
                        motorNeedsCalibration = wMotorNeedsCalibration((wMotorType_t) m);
                    }
                    // If the movement resulted in the motor needing
                    // calibration, clear all of the steps for this
                    // motor and do that
                    if (motorNeedsCalibration) {
                        item->durationTicksList.clear();
                        if ((motorCalibrateAndMoveToRest((wMotorType_t) m,
                                                         calibrationAttemptFailureCount) == 0) &&
                            motorRecalibrateCountTicks) {
                            // Reset the timed recalibration counter
                            // as we've done that
                            *motorRecalibrateCountTicks = 0;
                        }
                    } else {
                        // Set the duration to minus its value in order
                        // to start the count-down
                        durationTicks = -durationTicks;
                    }
                }
                // Increment the duration and if it is negative, i.e.
                // there is a wait of more than one tick, push it back
                // on the front of the queue to be dealt with on the
                // next tick
                durationTicks++;
                if (durationTicks < 0) {
                    item->durationTicksList.push_front(durationTicks);
                    movingAtEnd = true;
                } else if (!item->durationTicksList.empty()) {
                    // If this duration had hit zero but the duration
                    // tick list is not empty we haven't stopped moving
                    movingAtEnd = true;
                }
            }
        }

        if (movingAtStart && !movingAtEnd) {
            W_LOG_DEBUG_START("movement completed");
            if (intervalCountTicks) {
                W_LOG_DEBUG_MORE(", waiting at least %d ms.",
                                 W_CONTROL_MOTOR_MOVE_INTERVAL_MS);
                // If there was something on either list when we were called
                // but there is nothing on either list anymore then start
                // the interval counter
                *intervalCountTicks = 0;
            }
            W_LOG_DEBUG_MORE(".");
            W_LOG_DEBUG_END;
        }
    }

    return movingAtStart && movingAtEnd;
}

// Perform a motor movement (e.g. motorMoveToRest() or
// motorCalibrateAndMoveToRest()) on the basis of a counter and
// a limit. steps, if present, must be a pointer to an array
// of size W_MOTOR_TYPE_MAX_NUM.
static bool timedMotorMove(int *tickCount, int limitTicks,
                           int (*function)(wMotorType_t, unsigned int *),
                           unsigned int *calibrationAttemptFailureCount = nullptr,
                           std::atomic<int> *intervalCountTicks = nullptr,
                           wControlSteps_t *steps = nullptr)
{
    bool called = false;

    if (tickCount && (limitTicks > 0)) {
        (*tickCount)++;
        bool notYet = false;
        if (steps) {
            // Check if we are currently stepping, only rest after
            // those are done
            for (unsigned int m = 0; (m < W_MOTOR_TYPE_MAX_NUM) &&
                                     !notYet; m++) {
                if (!(steps + m)->durationTicksList.empty()) {
                    notYet = true;
                }
            }
        }
        // Check if we are in an interval; only rest after it is done
        if (!notYet && intervalCountTicks &&
            (*intervalCountTicks < msToTicks(W_CONTROL_MOTOR_MOVE_INTERVAL_MS))) {
            notYet = true;
        }
        if (!notYet && (*tickCount >= limitTicks)) {
            W_LOG_DEBUG_START("a tick count (%d second(s)) has expired,"
                              " performing a timed motor action",
                              ticksToMs(limitTicks) / 1000);
            if (intervalCountTicks) {
                // Need an interval count as we'll be moving
                W_LOG_DEBUG_MORE(" and waiting at least %d ms",
                                 W_CONTROL_MOTOR_MOVE_INTERVAL_MS);
                *intervalCountTicks = 0;
            }
            W_LOG_DEBUG_MORE(".");
            W_LOG_DEBUG_END;
            if (function) {
                for (unsigned int m = 0; m < W_MOTOR_TYPE_MAX_NUM; m++) {
                    // Don't care about errors here, best effort
                    function((wMotorType_t) m, calibrationAttemptFailureCount);
                    // Clear any steps that might be on-going
                    if (steps) {
                        (steps + m)->durationTicksList.clear();
                    }
                }
            }
            *tickCount = 0;
            called = true;
        }
    }

    return called;
}

// The control loop.
static void controlLoop()
{
    if (gTimerFd >= 0) {
        uint64_t numExpiries;
        struct pollfd pollFd[1] = {};
        struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
        sigset_t sigMask;

        pollFd[0].fd = gTimerFd;
        pollFd[0].events = POLLIN | POLLERR | POLLHUP;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);

        W_LOG_DEBUG("control loop has started.");

        int returnToRestCountTicks = 0;
        int inactivityReturnToRestCountTicks = 0;
        int motorRecalibrateCountTicks = 0;
        bool activityFlag = false;
        unsigned int calibrationAttemptFailureCount = 0;
        // These arrays have the same order as wMotorType_t, i.e.
        // 0 -> vertical/y, 1 -> horizontal/x
        wControlSteps_t steps[W_MOTOR_TYPE_MAX_NUM];
        unsigned int stepsPerPixelX100[W_MOTOR_TYPE_MAX_NUM] = {};

        // Populate the steps per pixel from the motors, making
        // very sure that the motors are calibrated first
        motorEnsureCalibration(&calibrationAttemptFailureCount);
        if (stepsPerPixelX100Set(stepsPerPixelX100) != 0) {
            // This is very unlikely indeed to fail but putting something
            // red in the warning in case it does
            W_LOG_ERROR("unable to get initial range for motors!");
        }

        while (gKeepGoing && wUtilKeepGoing()) {
            // Block waiting for our tick timer to go off or for
            // CTRL-C to land
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(gTimerFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {

                // Check for the need to return to the rest position
                // or recalibrate the motors.  It is done this way so that
                // both checks are always carried out and then, if the motor
                // recalibration timer is a multiple of the return to
                // rest timer, they will both expire but will cause
                // only a single interval count
                bool motorRecalibrate = timedMotorMove(&motorRecalibrateCountTicks,
                                                       msToTicks(W_CONTROL_MOTOR_RECALIBRATE_SECONDS * 1000),
                                                       motorCalibrateAndMoveToRest,
                                                       &calibrationAttemptFailureCount,
                                                       &(gContext.intervalCountTicks), steps);
                bool returnToRest = timedMotorMove(&returnToRestCountTicks,
                                                   msToTicks(W_CONTROL_RETURN_TO_REST_SECONDS * 1000),
                                                   motorMoveToRest,
                                                   &calibrationAttemptFailureCount,
                                                   &(gContext.intervalCountTicks), steps);
                if (!motorRecalibrate && !returnToRest) {
                    // No enforced rest/calibration: if there are steps to complete, do them.
                    gContext.moving = step(steps, gContext.staticCamera,
                                           &(gContext.intervalCountTicks),
                                           &motorRecalibrateCountTicks,
                                           &calibrationAttemptFailureCount);
                    if (motorRecalibrateCountTicks == 0) {
                        // If stepping resulted in a recalibration (i.e.
                        // motorRecalibrateCountTicks has been reset to zero),
                        // remember that so that we can update the step to 
                        // pixel ratio later.
                        motorRecalibrate = true;
                    }
                    if (gContext.moving) {
                        // Remove the focus point from the image while we move
                        wImageProcessingFocusSet(nullptr);
                        inactivityReturnToRestCountTicks = 0;
                        activityFlag = true;
                    } else {
                        if (activityFlag) {
                            // If there has been activity, check for the need to return to
                            // the rest position due to inactivity
                            if (timedMotorMove(&inactivityReturnToRestCountTicks,
                                               msToTicks(W_CONTROL_INACTIVITY_RETURN_TO_REST_SECONDS * 1000),
                                               motorMoveToRest,
                                               &calibrationAttemptFailureCount,
                                               &(gContext.intervalCountTicks), steps)) {
                                activityFlag = false;
                            }
                        }
                        // Take a copy of the current focus point with the
                        // context locked; all the other context parameters
                        // we use in here are marked as atomic
                        gContext.mutex.lock();
                        cv::Point focusPointView = gContext.focus.averagePointView;
                        gContext.mutex.unlock();

                        // Not moving, check the interval counter
                        if (gContext.intervalCountTicks >= msToTicks(W_CONTROL_MOTOR_MOVE_INTERVAL_MS)) {
                            // If there is no interval to wait,
                            // create a new step list if required
                            if (stepListSet(&focusPointView, stepsPerPixelX100, steps)) {
                                wLedModeConstantSet(W_LED_BOTH, 0, 100, 500);
                            }
                        } else {
                            if (gContext.intervalCountTicks == msToTicks(W_CONTROL_MOTOR_MOVE_GUARD_MS)) {
                                // Reset the motion detection in the image processing code
                                wImageProcessingResetMotionDetect();
                                // Restart averaging, otherwise we
                                // will end up moving towards something
                                // we have already moved towards, IYSWIM
                                gContext.mutex.lock();
                                wControlPointView_t *focus = &(gContext.focus);
                                focus->number = 0;
                                focus->totalPointView = {0, 0};
                                focus->oldestPointView = nullptr;
                                focus->averagePointView = {0, 0};
                                gContext.mutex.unlock();
                                focusPointView = focus->averagePointView;
                                W_LOG_DEBUG("focus point reset.");
                            }
                            // Increment the interval count
                            gContext.intervalCountTicks++;
                            if (gContext.intervalCountTicks == msToTicks(W_CONTROL_MOTOR_MOVE_INTERVAL_MS)) {
                                W_LOG_DEBUG("inter-movement wait (%d ms) now over.",
                                            W_CONTROL_MOTOR_MOVE_INTERVAL_MS);
                                wLedModeConstantSet(W_LED_BOTH, 0, 10, 500);
                            }
                        }

                        // Write the focus point on the image
                        wImageProcessingFocusSet(&focusPointView);
                    }
                } else {
                    // Returning to rest or recalibrating counts as activity
                    inactivityReturnToRestCountTicks = 0;
                }

                // Catch-all: if wMotorMoveToRest() failed anywhere it is called
                // above it is possible that stepsPerPixelX100Get(), and
                // hence stepsPerPixelX100Set(), will fail, so make sure
                // that all is good
                motorRecalibrate |= motorEnsureCalibration(&calibrationAttemptFailureCount);

                if (motorRecalibrate) {
                    // If a motor was recalibrated, re-compute the step to pixel ratio
                    // Don't flag an error here as there's little we can do about it,
                    // if the failure is due to calibration it wiil be caught next time
                    // around
                    stepsPerPixelX100Set(stepsPerPixelX100);
                }
            }
        }

        if (calibrationAttemptFailureCount > 0) {
            W_LOG_WARN("motor recalibration failed %d time(s) during operation.",
                       calibrationAttemptFailureCount);
        }

        // Not sure if this is necessary
        for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(steps); x++) {
            steps[x].durationTicksList.clear();
        }
    }

    W_LOG_DEBUG("control loop has exited.");
}

// Stop the control loop, close the timer and release the message queue.
static void cleanUp()
{
    if (gTimerFd >= 0) {
        gKeepGoing = false;
        if (gContext.thread.joinable()) {
            gContext.thread.join();
        }
        close(gTimerFd);
        gTimerFd = -1;
    }
    if (gMsgQueueId >= 0) {
        wMsgQueueStop(gMsgQueueId);
        gMsgQueueId = -1;
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wControlMsgBodyFocusChange_t
 * -------------------------------------------------------------- */

// Message handler for wControlMsgBodyFocusChange_t.
static void msgHandlerControlFocusChange(void *msgBody,
                                         unsigned int bodySize,
                                         void *context)
{
    wControlMsgBodyFocusChange_t *msg = &(((wControlMsgBody_t *) msgBody)->focusChange);
    wControlContext_t *controlContext = (wControlContext_t *) context;

    assert(bodySize == sizeof(*msg));

    if (!controlContext->moving &&
        (controlContext->intervalCountTicks > msToTicks(W_CONTROL_MOTOR_MOVE_GUARD_MS)) &&
        (msg->areaPixels >= W_CONTROL_FOCUS_AREA_THRESHOLD_PIXELS)) {
        // We're not moving, have been stationary for more than the guard
        // period, and the new point is big enough to be added to our
        // focus data
        controlContext->mutex.lock();

        wControlPointView_t *focus = &(controlContext->focus);
        cv::Point *pointView = &(msg->pointView);
        // Calculate the new total, and hence the average
        if (focus->oldestPointView == nullptr) {
            // Haven't yet filled the buffer up, just add the
            // new point and update the total
            focus->pointView[focus->number] = *pointView;
            focus->number++;
            focus->totalPointView += *pointView;
            if (focus->number >= W_UTIL_ARRAY_COUNT(focus->pointView)) {
                focus->oldestPointView = &(focus->pointView[0]);
            }
        } else {
            // The buffer is full, need to rotate it
            focus->totalPointView -= *focus->oldestPointView;
            *focus->oldestPointView = *pointView;
            focus->totalPointView += *pointView;
            focus->oldestPointView++;
            if (focus->oldestPointView >= focus->pointView + W_UTIL_ARRAY_COUNT(focus->pointView)) {
                focus->oldestPointView = &(focus->pointView[0]);
            }
        }

        if (focus->number > 0) {
            // Note: the average becomes an unsigned value unless the
            // denominator is cast to an integer
            focus->averagePointView = focus->totalPointView / (int) focus->number;
        }

        controlContext->mutex.unlock();
    }

}

/* ----------------------------------------------------------------
 * MORE VARIABLES: THE MESSAGES WITH THEIR MESSAGE HANDLERS
 * -------------------------------------------------------------- */

// Array of message handlers with the message type they handle.
static wControlMsgHandler_t gMsgHandler[] = {{.msgType = W_CONTROL_MSG_TYPE_FOCUS_CHANGE,
                                              .function = msgHandlerControlFocusChange}};

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise control.
int wControlInit()
{
    int errorCode = 0;

    if (gTimerFd < 0) {
        // Create our message queue
        errorCode = wMsgQueueStart(&gContext, W_CONTROL_MSG_QUEUE_MAX_SIZE, "control");
        if (errorCode >= 0) {
            gMsgQueueId = errorCode;
            errorCode = 0;
            // Register the message handler
            for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMsgHandler)) &&
                                     (errorCode == 0); x++) {
                wControlMsgHandler_t *handler = &(gMsgHandler[x]);
                errorCode = wMsgQueueHandlerAdd(gMsgQueueId,
                                                handler->msgType,
                                                handler->function);
            }
        }
        if (errorCode == 0) {
            // Set up a tick to drive controlLoop()
            errorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (errorCode >= 0) {
                gTimerFd = errorCode;
                errorCode = 0;
                struct itimerspec timerSpec = {};
                timerSpec.it_value.tv_nsec = W_CONTROL_TICK_TIMER_PERIOD_MS * 1000000;
                timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
                if (timerfd_settime(gTimerFd, 0, &timerSpec, nullptr) == 0) {
                    errorCode = 0;
                    // Start the control loop
                    try {
                        // This will go bang if the thread cannot be created
                        gKeepGoing = true;
                        gContext.thread = std::thread(controlLoop);
                    }
                    catch (int x) {
                        errorCode = -x;
                        W_LOG_ERROR("unable to start control tick thread, error code %d.",
                                    errorCode);
                    }
                } else {
                    errorCode = -errno;
                    W_LOG_ERROR("unable to set control tick timer, error code %d.",
                                errorCode);
                }
            } else {
                errorCode = -errno;
                W_LOG_ERROR("unable to create control tick timer, error code %d.",
                            errorCode);
            }
        }

        if (errorCode < 0) {
            cleanUp();
        }
    }

    return errorCode;
}

// Start control operations.
int wControlStart(bool staticCamera)
{
    int errorCode = -EBADF;

    if (gTimerFd >= 0) {
        errorCode = 0;
        gContext.staticCamera = staticCamera;
        gContext.intervalCountTicks = INT_MAX;
        // Set ourselves up as a consumer of focus from the image processing
        errorCode = wImageProcessingFocusConsume(focusCallback);
        if (errorCode == 0) {
            // Start video encoding
            errorCode = wVideoEncodeStart();
            if (errorCode == 0) {
                // We're up
                wLedModeConstantSet(W_LED_BOTH, 0, 10, 1000);
                wLedOverlayRandomBlinkSet(5);
            } else {
                 wImageProcessingFocusConsume(nullptr);
            }
        }
    }

    return errorCode;
}

// Stop control operations
int wControlStop()
{
    int errorCode = -EBADF;

    if (gTimerFd >= 0) {
        gContext.staticCamera = false;
        wImageProcessingFocusConsume(nullptr);
        errorCode = wVideoEncodeStop();
    }

    return errorCode;
}

// Deinitialise control.
void wControlDeinit()
{
    if (gTimerFd >= 0) {
        wControlStop();
        cleanUp();
    }
}

// End of file
