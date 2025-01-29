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

#ifndef _W_IMAGE_PROCESSING_H_
#define _W_IMAGE_PROCESSING_H_

// This API is dependent on cv::Point and w_common.h (for wCommonFrameFunction_t).
#include <opencv2/core/types.hpp>
#include <w_common.h>

/** @file
 * @brief The image processing API for the watchdog application; this
 * API is NOT thread-safe.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_IMAGE_PROCESSING_MSG_QUEUE_MAX_SIZE
/** The maximum number of messages allowed in the video processing
 * queue: not so many of these as the buffers are usually quite large,
 * we just need to keep up.
 */
# define W_IMAGE_PROCESSING_MSG_QUEUE_MAX_SIZE 100
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Function signature of a callback that may consume the focus
 * produced by the image processing, used by
 * wImageProcessingFocusConsume().
 *
 * @param pointView   the focus point in view coordinates; view
 *                    coordinates have their origin in the
 *                    centre of the screen with the axes just like
 *                    a conventional X/Y graph.
 * @param areaPixels  the area of the focus point in pixels.
 * @param context     the context parameter that was passed to
 (                    wImageProcessingFocusConsume().
 */
typedef int (wImageProcessingFocusFunction_t)(cv::Point pointView,
                                              int areaPixels,
                                              void *context);

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise image processing; if image processing is already
 * initialised this function will do nothing and return success.
 * wMsgInit() must have returned successfully before this is called.
 *
 * @return zero on success else negative error code.
 */
int wImageProcessingInit();

/** Become a consumer of the focus point that the image processing
 * code determines.  There can only be one consumer, a new call to
 * this function will replace any previous callback.  This focus
 * point is only a _potential_ focus point, it may be smoothed,
 * ignored, whatever by the consumer before it is fed back into
 * the image with a call to wImageProcessingFocusSet().
 *
 * @param focusCallback  the function that will be called when
 *                       the focus changes; the function should
 *                       store the new focus point and return
 *                       as quickly as possible.  Use nullptr to
 *                       remove a previous focus callback.
 * @param context        user context that will be passed to
 *                       focusCallback as its last parameter.
 * @return               zero on success else negative error code.
 */
int wImageProcessingFocusConsume(wImageProcessingFocusFunction_t *focusCallback,
                                 void *context = nullptr);

/** Set the focus point to be drawn on the processed image.
 *
 * @param pointView   a pointer to the focus point in view
 *                    coordinates (i.e just like an X/Y graph
 *                    with the origin in the centre of the screen),
 *                    use nullptr to set the focus point as
 *                    invalid (in which case it will not be
 *                    drawn on the image).
 * @return            zero on success else negative error code.
 */
int wImageProcessingFocusSet(const cv::Point *pointView);

/** Start image processing; this will call wCameraStart(),
 * providing it with a callback to obtain a flow of images,
 * and provide the processed frames to the callback function
 * given here.  wCameraInit() must have been called and returned
 * success for this function to succeed.
 *
 * @param outputCallback  the (e.g. video encode) function that will
 *                        be called when an image has been processed.
 *                        The function should queue the image
 *                        [e.g. for encoding] and return as quickly
 *                        as possible.
 * @return                zero on success else negative error code.
 */
int wImageProcessingStart(wCommonFrameFunction_t *outputCallback);

/** Reset the motion detector: useful if the camera is moved.
 */
void wImageProcessingResetMotionDetect();

/** Stop image processing; you do not have to call this function
 * on exit, wImageProcessingDeinit() will tidy up appropriately.
 *
 * @return zero on success else negative error code.
 */
int wImageProcessingStop();

/** Deinitialise image processing and free resources.
 */
void wImageProcessingDeinit();

#endif // _W_IMAGE_PROCESSING_H_

// End of file
