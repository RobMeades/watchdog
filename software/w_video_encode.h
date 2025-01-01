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

#ifndef _W_VIDEO_ENCODE_H_
#define _W_VIDEO_ENCODE_H_

// This API is dependent on std::string (used by wVideoEncodeInit()).
#include <string>

/** @file
 * @brief The video encoding API for the watchdog application; this
 * API is NOT thread-safe.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_VIDEO_ENCODE_MSG_QUEUE_MAX_SIZE
/** The maximum number of frames allowed in the video processing
 * queue; lots of room needed.
 */
# define W_VIDEO_ENCODE_MSG_QUEUE_MAX_SIZE 1000
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise video encoding; if video encoding is already
 * initialised this function will do nothing and return success.
 * wMsgInit() must have returned successfully before this is called.
 *
 * @param outputDirectory    the output directory (with no trailing
 *                           slash).
 * @param outputFileName     the output file name, with no
 *                           extension.
 * @return                   zero on success else negative error code.
 */
int wVideoEncodeInit(std::string outputDirectory, std::string outputFileName);

/** Start video encoding; this will call wImageProcessingStart(),
 * providing it with a callback to obtain a flow of processed
 * images.  wImageProcessingInit() must have been called and returned
 * success for this function to succeed.
 *
 * @return zero on success else negative error code.
 */
int wVideoEncodeStart();

/** Stop video encoding; you don't have to call this function
 * on exit, wVideoEncodeDeinit() will tidy up appropriately.
 *
 * @return zero on success else negative error code.
 */
int wVideoEncodeStop();

/** Deinitialise video encoding and free resources.
 */
void wVideoEncodeDeinit();

#endif // _W_VIDEO_ENCODE_H_

// End of file
