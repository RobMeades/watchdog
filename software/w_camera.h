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

#ifndef _W_CAMERA_H_
#define _W_CAMERA_H_

// This API is dependent on w_common.h (for wCommonFrameFunction_t,
// W_COMMON_FRAME_RATE_HERTZ, W_COMMON_WIDTH_PIXELS and W_COMMON_HEIGHT_PIXELS).
#include <w_common.h>

/** @file
 * @brief The camera API for the watchdog application; this API is
 * NOT thread-safe.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_CAMERA_STREAM_FORMAT
/** The pixel format for the video stream: must be YUV420 as that is
 * what the code is expecting.
 */
# define W_CAMERA_STREAM_FORMAT "YUV420"
#endif

#ifndef W_CAMERA_WIDTH_PIXELS
/** Horizontal size of video stream in pixels.
 */
# define W_CAMERA_WIDTH_PIXELS W_COMMON_WIDTH_PIXELS
#endif

#ifndef W_CAMERA_HEIGHT_PIXELS
/** Vertical size of the video stream in pixels.
 */
# define W_CAMERA_HEIGHT_PIXELS W_COMMON_HEIGHT_PIXELS
#endif

/** The area of the video stream.
 */
#define W_CAMERA_AREA_PIXELS (W_CAMERA_WIDTH_PIXELS * \
                                     W_CAMERA_HEIGHT_PIXELS)

#ifndef W_CAMERA_FRAME_RATE_HERTZ
/** Frames per second.
 */
# define W_CAMERA_FRAME_RATE_HERTZ W_COMMON_FRAME_RATE_HERTZ
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise the camera.
 *
 * @return zero on success else negative error code.
 */
int wCameraInit();

/** Start the camera; this will cause video frames to be sent
 * to the callback function provided.
 *
 * @param outputCallback  the (e.g. image processing) function that
 *                        will be called when a frame is available
 *                        from the camera.  The function should queue
 *                        the image for processing and return as
 *                        quickly as possible.
 * @return                zero on success else negative error code.
 */
int wCameraStart(wCommonFrameFunction_t *outputCallback);

/** Get the current frame count of the camera.
 *
 * @return  the frame count; zero if the camera is not running.
 */
uint64_t wCameraFrameCountGet();

/** Stop the camera; there is no need to call this, wCameraDeinit()
 * will perform all necessary clean-up in any case.
 *
 * @return zero on success else negative error code.
 */
int wCameraStop();

/** Deinitialise the camera.
 */
void wCameraDeinit();

/** List the available cameras and their properties; this should
 * be called while the camera is NOT initialised to print useful
 * information about the available camera hardware.
 *
 * @return the number of cameras found (hopefully at least one)
 *         or negative error code.
 */
int wCameraList();

#endif // _W_CAMERA_H_

// End of file
