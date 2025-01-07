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

/** Context for control message handlers.
 */
typedef struct {
    int fd;
    std::thread thread;
    std::mutex mutex;
    bool staticCamera;
    wControlPointView_t focus;
    int intervalCountTicks;
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

// Update where we're looking.
static bool move(const cv::Point *focus, bool staticCamera)
{
    int stepsTakenVertical = 0;
    int stepsTakenRotate = 0;

    if (focus) {
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
        // here we drive the motors in order to make the average
        // become closer to 0, 0.
        W_LOG_DEBUG("focus %d, %d.", focus->x, focus->y);
        int x = 0;
        if (focus->x > W_CONTROL_FOCUS_ROTATE_INCREMENT_STEPS) {
            // Look further right
            x = W_CONTROL_FOCUS_ROTATE_INCREMENT_STEPS;
            if (!staticCamera) {
                wMotorMove(W_MOTOR_TYPE_ROTATE, x, &stepsTakenRotate);
            }
        } else if (focus->x < -W_CONTROL_FOCUS_ROTATE_INCREMENT_STEPS) {
            // look further left
            x = -W_CONTROL_FOCUS_ROTATE_INCREMENT_STEPS;
            if (!staticCamera) {
                wMotorMove(W_MOTOR_TYPE_ROTATE, x, &stepsTakenRotate); 
            }
        }
        if (staticCamera) {
            W_LOG_DEBUG("x: would move %d step(s).", x);
        }
        int y = 0;
        if (focus->y > W_CONTROL_FOCUS_VERTICAL_INCREMENT_STEPS) {
            // Look further up
            y = W_CONTROL_FOCUS_VERTICAL_INCREMENT_STEPS;
            if (!staticCamera) {
                wMotorMove(W_MOTOR_TYPE_VERTICAL, y, &stepsTakenVertical);
            }
        } else if (focus->y < -W_CONTROL_FOCUS_VERTICAL_INCREMENT_STEPS) {
            // look further down
            y = -W_CONTROL_FOCUS_VERTICAL_INCREMENT_STEPS;
            if (!staticCamera) {
                wMotorMove(W_MOTOR_TYPE_VERTICAL, y, &stepsTakenVertical);
            }
        }
        if (staticCamera) {
            W_LOG_DEBUG("y: would move %d step(s).", y);
        }
    }

    return ((stepsTakenRotate != 0) || (stepsTakenVertical != 0));
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

        while (gKeepGoing && wUtilKeepGoing()) {
            // Block waiting for our tick timer to go off or for
            // CTRL-C to land
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(gTimerFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {

                gContext.mutex.lock();
                // Take a copy of the average focus point so that
                // we can do the moving, which may take a while,
                // outside of the context lock
                cv::Point focus = gContext.focus.averagePointView;
                gContext.mutex.unlock();

                // It is OK to update intervalCountTicks without
                // a lock on the context as this function is the
                // only one writing to it.
                if (gContext.intervalCountTicks == 0) {
                    // Update where we're looking
                    if (move(&focus, gContext.staticCamera)) {
                        gContext.intervalCountTicks = W_CONTROL_FOCUS_MOVE_INTERVAL_TICKS;
                    }
                } else {
                    // Decrement any hysteresis count
                    gContext.intervalCountTicks--;
                }

                // Write the focus point on the image
                wImageProcessingFocusSet(&focus);
            }
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

    if ((controlContext->intervalCountTicks == 0) &&
        (msg->areaPixels >= W_CONTROL_FOCUS_THRESHOLD_AREA_PIXELS)) {
        // We're not in a hysteresis period (when the focus may be moving
        // around due to the motion of the watchdog's head) and the new
        // point is big enough to be added to our focus data
        controlContext->mutex.lock();

        wControlPointView_t *focus = &(controlContext->focus);
        cv::Point *pointView = &(msg->pointView);
        // Calculate the new total, and hence the average
        if (focus->oldestPointView == NULL) {
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
        gContext.staticCamera = staticCamera;
        // Set ourselves up as a consumer of focus from the image processing
        errorCode = wImageProcessingFocusConsume(focusCallback);
        if (errorCode == 0) {
            // Start video encoding
            errorCode = wVideoEncodeStart();
            if (errorCode == 0) {
                // We're breathing
                wLedModeBreatheSet();
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
