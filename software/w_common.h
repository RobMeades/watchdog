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

#ifndef _W_COMMON_H_
#define _W_COMMON_H_

/** NO #inclusions allowed in here, since they would spread everywhere.
 */

/** @file
 * @brief Types shared amongst all of the APIs in the watchdog application.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_COMMON_WIDTH_PIXELS
/** Horizontal size of video stream in pixels.
 */
# define W_COMMON_WIDTH_PIXELS 950
#endif

#ifndef W_COMMON_HEIGHT_PIXELS
/** Vertical size of the video stream in pixels.
 */
# define W_COMMON_HEIGHT_PIXELS 540
#endif

#ifndef W_COMMON_FRAME_RATE_HERTZ
/** Frames per second.
 */
# define W_COMMON_FRAME_RATE_HERTZ 15
#endif

#ifndef W_COMMON_THREAD_REAL_TIME_PRIORITY_MAX
/** The maximum real-time priority to use for any of the threads,
 * where Linux/Posix defines 100 as the maximum real-time
 * priority and 0 as the lowest real-time priority.  The members
 * of wCommonThreadPriority_t and the macro
 * W_COMMON_THREAD_REAL_TIME_PRIORITY() can be used to set the priority
 * of the real-time threads in this process relative to this
 * value.
 */
# define W_COMMON_THREAD_REAL_TIME_PRIORITY_MAX 50
#endif

/** Macro to obtain a real-time thread priority that can be used
 * with sched_setscheduler().  The priority parameter should be
 * a value from wCommonThreadPriority_t.
 */
# define W_COMMON_THREAD_REAL_TIME_PRIORITY(priority) (W_COMMON_THREAD_REAL_TIME_PRIORITY_MAX + priority)

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Thread priorities, for the threads which need one, relative to
 * W_COMMON_THREAD_REAL_TIME_PRIORITY_MAX (so 0 is
 * W_COMMON_THREAD_REAL_TIME_PRIORITY_MAX, -1 is one lower in
 * priority, etc.), stored in this enum so that they are all in one
 * place rather than scattered about the header files.  Use a value
 * from this enum with W_COMMON_THREAD_REAL_TIME_PRIORITY() to get
 * a real-time thread priority that can be passed to Linux.
 */
typedef enum {
    W_COMMON_THREAD_PRIORITY_GPIO_READ = 0,
    W_COMMON_THREAD_PRIORITY_GPIO_PWM = -1,
    W_COMMON_THREAD_PRIORITY_LED = -2,
    W_COMMON_THREAD_PRIORITY_CONTROL = -3,
    W_COMMON_THREAD_PRIORITY_MSG = -4
} wCommonThreadPriority_t;

/** Function signature of something that processes a frame, used
 * by the camera and image processing APIs.
 *
 * @param data         a pointer to the image data; this data will
 *                     have been malloc()'ed and should be free()'ed
 *                     by the frame processing function.
 * @param length       the amount of memory pointed to by data.
 * @param sequence     a monotonically-increasing sequence number.
 * @param width        the width of the image in pixels.
 * @param height       the height of the image in pixels.
 * @param stride       the stride of the image, i.e, the width
 *                     plus any packing.
 * @return             the number of frames now in the queue for
 *                     processing (i.e. the backlog).
 */
typedef int (wCommonFrameFunction_t)(uint8_t *data,
                                     unsigned int length,
                                     unsigned int sequence,
                                     unsigned int width,
                                     unsigned int height,
                                     unsigned int stride);

#endif // _W_COMMON_H_

// End of file
