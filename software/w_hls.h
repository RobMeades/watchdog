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

#ifndef _W_HLS_H_
#define _W_HLS_H_

// This API is dependent on w_util.h.
#include <w_util.h>

/** @file
 * @brief The HLS (HTTP Live Stream) API for the watchdog application.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */


#ifndef W_HLS_FILE_NAME_ROOT_DEFAULT
/** The default root name for our HLS video files (.m3u8 and .ts).
 */
# define W_HLS_FILE_NAME_ROOT_DEFAULT "watchdog"
#endif

#ifndef W_HLS_PLAYLIST_FILE_EXTENSION
/** Playlist file extension.
 */
# define W_HLS_PLAYLIST_FILE_EXTENSION ".m3u8"
#endif

#ifndef W_HLS_SEGMENT_FILE_EXTENSION
/** Segment file extension.
 */
# define W_HLS_SEGMENT_FILE_EXTENSION ".ts"
#endif

#ifndef W_HLS_OUTPUT_DIRECTORY_DEFAULT
/** The default output directory; should NOT end in a "/".
 */
# define W_HLS_OUTPUT_DIRECTORY_DEFAULT W_UTIL_DIR_THIS
#endif

#ifndef W_HLS_SEGMENT_DURATION_SECONDS
/** The duration of a segment in seconds.
 */
# define W_HLS_SEGMENT_DURATION_SECONDS 2
#endif

#ifndef W_HLS_LIST_SIZE
/** The number of segments in the list.
 */
# define W_HLS_LIST_SIZE 15
#endif

#ifndef W_HLS_BASE_URL
/** The URL to serve from (must NOT end with a "/").
 */
# define W_HLS_BASE_URL "http://10.10.1.16"
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

#endif // _W_HLS_H_

// End of file
