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

// The OpenCV stuff (for cv::Point).
#include <opencv2/core/types.hpp>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>
#include <w_msg.h>
#include <w_image_processing.h>
#include <w_video_encode.h>

// Us.
#include <w_control.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Control message types; just the one.
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

// The ID of the control message queue
static int gMsgQueueId = -1;

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

// Release the message queue.
static void cleanUp()
{
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

    assert(bodySize == sizeof(*msg));

    // This handler doesn't use any context
    (void) context;

    // Set the focus point on the image
    wImageProcessingFocusSet(&(msg->pointView));
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

    if (gMsgQueueId < 0) {
        // Create our message queue
        errorCode = wMsgQueueStart(nullptr, W_CONTROL_MSG_QUEUE_MAX_SIZE, "control");
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

        if (errorCode < 0) {
            cleanUp();
        }
    }

    return errorCode;
}

// Start control operations.
int wControlStart()
{
    int errorCode = -EBADF;

    if (gMsgQueueId >= 0) {
        // Set ourselves up as a consumer of focus from the image processing
        errorCode = wImageProcessingFocusConsume(focusCallback);
        if (errorCode == 0) {
            // Start video encoding
            errorCode = wVideoEncodeStart();
            if (errorCode != 0) {
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

    if (gMsgQueueId >= 0) {
        wImageProcessingFocusConsume(nullptr);
        errorCode = wVideoEncodeStop();
    }

    return errorCode;
}

// Deinitialise control.
void wControlDeinit()
{
    if (gMsgQueueId >= 0) {
        wVideoEncodeStop();
        wImageProcessingFocusConsume(nullptr);
        cleanUp();
    }
}

// End of file
