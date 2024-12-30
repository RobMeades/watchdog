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
 * @brief The watchdog application, main().  This would normally be
 * split into multiple files with proper APIs between the image processing,
 * image streaming and control parts (there are queues between them for
 * this purpose) but since I'm editing on a PC and running on a
 * headless Raspberry Pi, having a single .cpp file that I can sftp
 * between the two makes life a lot simpler  Sorry software gods.
 *
 * This code makes use of:
 *
 * - libcamera: shiny new CPP stuff that is the only way to access
 *   Pi Camera 3, with fair API documentation but zero "how to"
 *   documentation,
 * - OpenCV: older CPP stuff, used here to process still images, find
 *   things that have moved between two still images, write to images,
 *   with good tutorial-style documentation but close to zero API
 *   documentation,
 * - FFmpeg: traditional C code, been around forever, used here only to
 *   encode a HLS-format video output stream; appalling documentation
 *   (99% of people seem to use it via the command-line) and support
 *   only via IRC.
 * - libgpiod: to read/write GPIO pins.  Note that the Raspberry Pi 5
 *   is different to all of the other Pis where GPIOs are concerned,
 *   see http://git.munts.com/muntsos/doc/AppNote11-link-gpiochip.pdf,
 *   in case this happens again.
 *
 * Note: to run with maximum debug from libcamera, execute this program
 * as:
 *
 * LIBCAMERA_LOG_LEVELS=0 sudo ./watchdog
 *
 * ...or to switch all debug output off:
 *
 * LIBCAMERA_LOG_LEVELS=3 sudo ./watchdog
 *
 * The default is to run with  log level 1, which includes
 * information, warning and errors from libcamera, but not pure debug.
 */

// The CPP stuff.
#include <string>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <list>
#include <atomic>
#include <chrono>

// The Linux/Posix stuff.
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>

// The libcamera stuff.
#include <libcamera/libcamera.h>

// The OpenCV stuff.
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/video.hpp>

extern "C" {
// The FFMPEG stuff, in good 'ole C.
# include <libavformat/avformat.h>
# include <libavcodec/avcodec.h>
# include <libavdevice/avdevice.h>
# include <libavutil/imgutils.h>
}

// Watchdog bits
#include <w_util.h>
#include <w_log.h>
#include <w_command_line.h>
#include <w_gpio.h>
#include <w_msg.h>
#include <w_led.h>
#include <w_motor.h>
#include <w_hls.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: MISC
 * -------------------------------------------------------------- */

// Print the duration of an operation for debug purposes.
#define W_PRINT_DURATION(x) auto _t1 = std::chrono::high_resolution_clock::now();  \
                            x;                                                     \
                            auto _t2 = std::chrono::high_resolution_clock::now();  \
                            W_LOG_DEBUG("%d ms to do \"" #x "\".",                 \
                                        std::chrono::duration_cast<std::chrono::milliseconds>(_t2 - _t1))

#ifndef W_MONITOR_TIMING_LENGTH
// The number of frames to average timing over when monitoring.
# define W_MONITOR_TIMING_LENGTH 1000
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: CAMERA RELATED
 * -------------------------------------------------------------- */

#ifndef W_CAMERA_STREAM_ROLE
// The libcamera StreamRole to use as a basis for the video stream.
# define W_CAMERA_STREAM_ROLE libcamera::StreamRole::VideoRecording
#endif

#ifndef W_CAMERA_STREAM_FORMAT
// The pixel format for the video stream: must be YUV420 as that is
// what the code is expecting.
# define W_CAMERA_STREAM_FORMAT "YUV420"
#endif

#ifndef W_CAMERA_STREAM_WIDTH_PIXELS
// Horizontal size of video stream in pixels.
# define W_CAMERA_STREAM_WIDTH_PIXELS 950
#endif

#ifndef W_CAMERA_STREAM_HEIGHT_PIXELS
// Vertical size of the video stream in pixels.
# define W_CAMERA_STREAM_HEIGHT_PIXELS 540
#endif

// The area of the video stream.
#define W_CAMERA_STREAM_AREA_PIXELS (W_CAMERA_STREAM_WIDTH_PIXELS * \
                                     W_CAMERA_STREAM_HEIGHT_PIXELS)

#ifndef W_CAMERA_FRAME_RATE_HERTZ
// Frames per second.
# define W_CAMERA_FRAME_RATE_HERTZ 15
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: VIEW/POINT RELATED
 * -------------------------------------------------------------- */

// View coordinates have their origin at the centre of the screen,
// as opposed to OpenCV which has its origin top left.

// Where the top of the screen lies.
#define W_VIEW_TOP ((W_CAMERA_STREAM_HEIGHT_PIXELS - 1) / 2)

// Where the bottom of the screen lies.
#define W_VIEW_BOTTOM -W_VIEW_TOP

// Where the right of the screen lies.
#define W_VIEW_RIGHT ((W_CAMERA_STREAM_WIDTH_PIXELS - 1) / 2)

// Where the left of the screen lies.
#define W_VIEW_LEFT -W_VIEW_RIGHT

// The view point origin in terms of an OpenCV frame.
#define W_VIEW_ORIGIN_AS_FRAME cv::Point{(W_CAMERA_STREAM_WIDTH_PIXELS - 1) / 2, \
                                         (W_CAMERA_STREAM_HEIGHT_PIXELS - 1) / 2}

// The OpenCV frame point origin in terms of our view coordinates.
#define W_FRAME_ORIGIN_AS_VIEW cv::Point{W_VIEW_LEFT, W_VIEW_TOP}

// The x or y-coordinate of an invalid point.
#define W_POINT_COORDINATE_INVALID -INT_MAX

// An invalid view point.
#define W_POINT_INVALID cv::Point{W_POINT_COORDINATE_INVALID, \
                                  W_POINT_COORDINATE_INVALID}

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: DRAWING RELATED
 * -------------------------------------------------------------- */

#ifndef W_DRAWING_SHADE_WHITE
// White, to be included in an OpenCV Scalar for an 8-bit gray-scale
// image.
# define W_DRAWING_SHADE_WHITE 255
#endif

#ifndef W_DRAWING_SHADE_BLACK
// Black, to be included in an OpenCV Scalar.
# define W_DRAWING_SHADE_BLACK 0
#endif

#ifndef W_DRAWING_SHADE_LIGHT_GRAY
// Light gray, to be included in an OpenCV Scalar for an 8-bit
// gray-scale image.
# define W_DRAWING_SHADE_LIGHT_GRAY 200
#endif

#ifndef W_DRAWING_SHADE_MID_GRAY
// Mid gray, to be included in an OpenCV Scalar for an 8-bit
// gray-scale image.
# define W_DRAWING_SHADE_MID_GRAY 128
#endif

// The colour we draw around moving objects as an OpenCV Scalar.
#define W_DRAWING_SHADE_MOVING_OBJECTS cv::Scalar(W_DRAWING_SHADE_MID_GRAY, \
                                                  W_DRAWING_SHADE_MID_GRAY, \
                                                  W_DRAWING_SHADE_MID_GRAY)

// The colour we draw the focus circle in as an OpenCV Scalar.
#define W_DRAWING_SHADE_FOCUS_CIRCLE cv::Scalar(W_DRAWING_SHADE_LIGHT_GRAY, \
                                                W_DRAWING_SHADE_LIGHT_GRAY, \
                                                W_DRAWING_SHADE_LIGHT_GRAY)

#ifndef W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS
// The line thickness we use when drawing rectangles around moving
// objects: 5 is nice and chunky.
# define W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS 5
#endif

#ifndef W_DRAWING_LINE_THICKNESS_FOCUS_CIRCLE
// The line thickness for the focus circle; should be less chunky than
// the moving objects so as not to obscure anything.
# define W_DRAWING_LINE_THICKNESS_FOCUS_CIRCLE 1
#endif

#ifndef W_DRAWING_RADIUS_FOCUS_CIRCLE
// The radius of the focus circle when we draw it on the screen.
# define W_DRAWING_RADIUS_FOCUS_CIRCLE  150
#endif

// The colour we draw the date/time text.
#define W_DRAWING_DATE_TIME_TEXT_SHADE cv::Scalar(W_DRAWING_SHADE_BLACK, \
                                                  W_DRAWING_SHADE_BLACK, \
                                                  W_DRAWING_SHADE_BLACK)

#ifndef W_DRAWING_DATE_TIME_TEXT_THICKNESS
// The thickness of the line drawing the date/time text: 1 is
// perfectly readable.
# define W_DRAWING_DATE_TIME_TEXT_THICKNESS 1
#endif

#ifndef W_DRAWING_DATE_TIME_FONT_HEIGHT
// Font height scale for the date/time stamp: 0.5 is nice and small
// and fits within the rectangle set out below.
# define W_DRAWING_DATE_TIME_FONT_HEIGHT 0.5
#endif

#ifndef W_DRAWING_DATE_TIME_HEIGHT_PIXELS
// The height of the date/time box in pixels: 20 is high enough for
// text height set to 0.5 with a small margin.
# define W_DRAWING_DATE_TIME_HEIGHT_PIXELS 20
#endif

#ifndef W_DRAWING_DATE_TIME_WIDTH_PIXELS
// The width of the date/time box in pixels: 190 is wide enough for
// a full %Y-%m-%d %H:%M:%S date/time stamp with font size 0.5 and
// a small margin.
# define W_DRAWING_DATE_TIME_WIDTH_PIXELS 190
#endif

#ifndef W_DRAWING_DATE_TIME_MARGIN_PIXELS_X
// Horizontal margin for the text inside its date/time box: 2 is good.
# define W_DRAWING_DATE_TIME_MARGIN_PIXELS_X 2
#endif

#ifndef W_DRAWING_DATE_TIME_MARGIN_PIXELS_Y
// Vertical margin for the text inside its date/time box: needs to be
// more than the X margin to look right, 5 is good.
# define W_DRAWING_DATE_TIME_MARGIN_PIXELS_Y 5
#endif

// The background colour we draw the rectangle that the date/time is
// printed on.
#define W_DRAWING_DATE_TIME_REGION_SHADE cv::Scalar(W_DRAWING_SHADE_WHITE, \
                                                    W_DRAWING_SHADE_WHITE, \
                                                    W_DRAWING_SHADE_WHITE)

#ifndef W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_X
// Horizontal offset for the date/time box when it is placed into the
// main image: 5 will position it nicely on the left. 
# define W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_X 5
#endif

#ifndef W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_Y
// Vertical offset for the date/time box when it is placed into the
// main image: 5 will position it nicely at the bottom (i.e. this is
// taken away from the image height when making an OpenCV Point). 
# define W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_Y 5
#endif

#ifndef W_DRAWING_DATE_TIME_ALPHA
// Opacity, AKA alpha, of the date/time box on top of the main image,
// range 1 to 0. 0.7 is readable but you can still see the
// underlying image.
# define W_DRAWING_DATE_TIME_ALPHA 0.7
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: VIDEO-CODING RELATED
 * -------------------------------------------------------------- */

// The stream time-base as an AVRational (integer pair, numerator
// then denominator) that FFmpeg understands.
#define W_VIDEO_STREAM_TIME_BASE_AVRATIONAL {1, W_CAMERA_FRAME_RATE_HERTZ}

// The video stream frame rate in units of the video stream time-base.
#define W_VIDEO_STREAM_FRAME_RATE_AVRATIONAL {W_CAMERA_FRAME_RATE_HERTZ, 1}

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: LED RELATED
 * -------------------------------------------------------------- */

#ifndef W_LED_MORSE_MAX_SIZE
// The maximum length of a morse message to be flashed by an
// LED, including room for a null terminator.
# define W_LED_MORSE_MAX_SIZE (32 + 1)
#endif

#ifndef W_LED_MORSE_DURATION_DOT_MS
// The default duration of a dot when an LED is flashing morse,
// in milliseconds.
# define W_LED_MORSE_DURATION_DOT_MS 100
#endif

#ifndef W_LED_MORSE_DURATION_DASH_MS
// The default duration of a dash when an LED is flashing morse,
// in milliseconds.
# define W_LED_MORSE_DURATION_DASH_MS 500
#endif

#ifndef W_LED_MORSE_DURATION_GAP_LETTER_MS
// The default duration of a gap between each letter when an LED
// is flashing morse, in milliseconds.
# define W_LED_MORSE_DURATION_GAP_LETTER_MS 100
#endif

#ifndef W_LED_MORSE_DURATION_GAP_WORD_MS
// The default duration of a gap between words when an LED is
// flashing morse, in milliseconds.
# define W_LED_MORSE_DURATION_GAP_WORD_MS 500
#endif

#ifndef W_LED_MORSE_DURATION_GAP_REPEAT_MS
// The default duration of a gap between repeats when an LED is
// flashing morse repeatedly, in milliseconds.
# define W_LED_MORSE_DURATION_GAP_REPEAT_MS 1000
#endif

#ifndef W_LED_TICK_TIMER_PERIOD_MS
// The LED tick timer period in milliseconds.
# define W_LED_TICK_TIMER_PERIOD_MS 20
#endif

#ifndef W_LED_RANDOM_BLINK_DURATION_MS
// The default duration of a blink in milliseconds.
# define W_LED_RANDOM_BLINK_DURATION_MS 100
#endif

#ifndef W_LED_RANDOM_BLINK_RANGE_SECONDS
// The default range of variation on a random blink interval in seconds.
# define W_LED_RANDOM_BLINK_RANGE_SECONDS 10
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: MESSAGING RELATED
 * -------------------------------------------------------------- */

#ifndef W_MSG_QUEUE_MAX_NUM_HANDLERS
// The maximum number of message handlers for any given queue.
# define W_MSG_QUEUE_MAX_NUM_HANDLERS 10
#endif

#ifndef W_MSG_QUEUE_MAX_SIZE_CONTROL
// The maximum number of message in the control message queue.
# define W_MSG_QUEUE_MAX_SIZE_CONTROL 100
#endif

#ifndef W_MSG_QUEUE_MAX_SIZE_IMAGE_PROCESSING
// The number of messages in the video processing queue: not so
// many of these as the buffers are usually quite large, we just
// need to keep up.
# define W_MSG_QUEUE_MAX_SIZE_IMAGE_PROCESSING 100
#endif

#ifndef W_MSG_QUEUE_MAX_SIZE_VIDEO_ENCODE
// The maximum size of the video frame queue; lots of room needed.
# define W_MSG_QUEUE_MAX_SIZE_VIDEO_ENCODE 1000
#endif

#ifndef W_MSG_QUEUE_MAX_SIZE_LED
// The maximum number of message in the LED message queue.
# define W_MSG_QUEUE_MAX_SIZE_LED 10
#endif

#ifndef W_MSG_QUEUE_TRY_LOCK_WAIT
// How long to wait for a mutex lock when pulling a message off a
// a queue (see also W_MSG_QUEUE_TICK_TIMER_PERIOD below).  This
// should be relatively long, we only need the timeout to go
// check if the loop should exit.
# define W_MSG_QUEUE_TRY_LOCK_WAIT std::chrono::seconds(1)
#endif

#ifndef W_MSG_QUEUE_TICK_TIMER_PERIOD_US
// The interval between polls for a lock on the mutex of a queue
// in microseconds.
# define W_MSG_QUEUE_TICK_TIMER_PERIOD_US 1000
#endif

/* ----------------------------------------------------------------
 * TYPES: MONITORING RELATED
 * -------------------------------------------------------------- */

// Structure to monitor timing.
typedef struct {
    std::chrono::time_point<std::chrono::high_resolution_clock> previousTimestamp;
    std::chrono::duration<double> gap[W_MONITOR_TIMING_LENGTH];
    unsigned int numGaps;
    // This is  non-NULL only when duration = W_MONITOR_TIMING_LENGTH
    std::chrono::duration<double> *oldestGap;
    std::chrono::duration<double> total;
    std::chrono::duration<double> largest;
    std::chrono::duration<double> average;
} wMonitorTiming_t;

/* ----------------------------------------------------------------
 * TYPES: IMAGE RELATED
 * -------------------------------------------------------------- */

// A point with mutex protection, used for the focus point which
// we need to write from the control thread and read from the
// requestCompleted() callback.  Aside from static initialisation,
// pointProtectedSet() and pointProtectedGet() should always be
// used to access a variable of this type.
typedef struct {
    std::mutex mutex;
    cv::Point point;
} wPointProtected_t;

// Hold information on a rectangle, likely one bounding an
// object we think is moving.
typedef struct {
    int areaPixels;
    cv::Point centreFrame; // The centre in frame coordinates
} wRectInfo_t;

/* ----------------------------------------------------------------
 * TYPES: FFMPEG RELATED
 * -------------------------------------------------------------- */

// Context needed by the video encode message handler.
typedef struct {
    AVFormatContext *formatContext;
    AVCodecContext *codecContext;
} wVideoEncodeContext_t;

/* ----------------------------------------------------------------
 * VARIABLES: CAMERA RELATED
 * -------------------------------------------------------------- */

// Pointer to camera: global as the requestCompleted() callback will use it.
static std::shared_ptr<libcamera::Camera> gCamera = nullptr;

// Count of frames received from the camera, purely for information.
static unsigned int gCameraStreamFrameCount = 0;

/* ----------------------------------------------------------------
 * VARIABLES: IMAGE PROCESSING RELATED
 * -------------------------------------------------------------- */

// The ID of the image processing message queue
static unsigned int gMsgQueueImageProcessing = -1;

// Pointer to the OpenCV background subtractor: global as the
// requestCompleted() callback will use it.
static std::shared_ptr<cv::BackgroundSubtractor> gBackgroundSubtractor = nullptr;

// A place to store the foreground mask for the OpenCV stream,
// global as the requestCompleted() callback will populate it.
static cv::Mat gMaskForeground;

// The place that we should be looking, in view coordinates.
// Note: use pointProtectedSet() to set this variable and
// pointProtectedGet() to read it.
static wPointProtected_t gFocusPointView = {.point = {0, 0}};

/* ----------------------------------------------------------------
 * VARIABLES: VIDEO RELATED
 * -------------------------------------------------------------- */

// The ID of the video encode message queue
static unsigned int gMsgQueueVideoEncode = -1;

// Count of frames received from the video codec, purely for
// information.
static unsigned int gVideoStreamFrameOutputCount = 0;

// Keep track of timing on the video stream, purely for information.
static wMonitorTiming_t gVideoStreamMonitorTiming;

/* ----------------------------------------------------------------
 * VARIABLES: CONTROL RELATED
 * -------------------------------------------------------------- */

// The ID of the control message queue
static unsigned int gMsgQueueControl = -1;

#if 0

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MONITORING RELATED
 * -------------------------------------------------------------- */

// Update a timing monitoring buffer.
static void monitorTimingUpdate(wMonitorTiming_t *monitorTiming)
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

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LIBCAMERA RELATED
 * -------------------------------------------------------------- */

// The conversion of a libcamera FrameBuffer to an OpenCV Mat requires
// the width, height and stride of the stream  So as to avoid having
// to search for this, we encode it into the cookie that is associated
// with a FrameBuffer when it is created, then the requestCompleted()
// callback can grab it.  See cookieDecode() for the reverse.
static uint64_t cookieEncode(unsigned int width, unsigned int height,
                             unsigned int stride)
{
    return ((uint64_t) width << 48) | ((uint64_t) (height & UINT16_MAX) << 32) |
            (stride & UINT32_MAX);
}

// Decode width, height and stride from a cookie; any pointer parameters
// may be NULL.
static void cookieDecode(uint64_t cookie, unsigned int *width,
                         unsigned int *height, unsigned int *stride)
{
    if (width != nullptr) {
        *width = (cookie >> 48) & UINT16_MAX;
    }
    if (height != nullptr) {
        *height = (cookie >> 32) & UINT16_MAX;
    }
    if (stride != nullptr) {
        *stride = cookie & UINT32_MAX;
    }
}

// Configure a stream from the camera.
static bool cameraStreamConfigure(libcamera::StreamConfiguration &streamCfg,
                                  std::string pixelFormatStr,
                                  unsigned int widthPixels,
                                  unsigned int heightPixels)
{
    bool formatFound = false;
    bool sizeFound = false;

    W_LOG_DEBUG("desired stream configuration %dx%d-%s.",
                widthPixels, heightPixels, pixelFormatStr.c_str());

    // Print out the current configuration
    W_LOG_DEBUG("existing stream configuration %s.",
                streamCfg.toString().c_str());

    // Set it up as we'd like
    streamCfg.pixelFormat = streamCfg.pixelFormat.fromString(pixelFormatStr);
    streamCfg.size.width = widthPixels;
    streamCfg.size.height = heightPixels;

    // Now go see what is possible and pick the nearest from those
    for (auto pixelFormat: streamCfg.formats().pixelformats()) {
        if (pixelFormat.toString().compare(pixelFormatStr) == 0) {
            formatFound = true;
            // Found the desired format, now find a size that
            // is as close as possible to the desired size.
            // This is relatively simple as the sizes are
            // given in increasing order
            for (auto size: streamCfg.formats().sizes(pixelFormat)) {
                if ((size.width >= streamCfg.size.width) &&
                    (size.height >= streamCfg.size.height)) {
                    streamCfg.size.width = size.width;
                    streamCfg.size.height = size.height;
                    sizeFound = true;
                    break;
                }
            }
        }
    }

    if (!formatFound) {
        W_LOG_ERROR_START("format %s not found, possible format(s): ",
                          pixelFormatStr.c_str());
        unsigned int x = 0;
        for (auto pixelFormat: streamCfg.formats().pixelformats()) {
            if (x > 0) {
                W_LOG_ERROR_MORE(", ");
            }
            W_LOG_ERROR_MORE(pixelFormat.toString().c_str());
            x++;
        }
        W_LOG_ERROR_MORE(".");
        W_LOG_ERROR_END;
    } else {
        if (!sizeFound) {
            W_LOG_ERROR_START("size %dx%d not found, possible size(s): ",
                              widthPixels, heightPixels);
            unsigned int x = 0;
            for (auto size: streamCfg.formats().sizes(streamCfg.pixelFormat)) {
                if (x > 0) {
                    W_LOG_ERROR_MORE(", ");
                }
                W_LOG_ERROR_MORE(size.toString().c_str());
                x++;
            }
            W_LOG_ERROR_MORE(".");
            W_LOG_ERROR_END;
        }
    }

    if (formatFound && sizeFound) {
        // Print where we ended up
        W_LOG_DEBUG("nearest stream configuration %s.",
                    streamCfg.toString().c_str());
    }

    return formatFound && sizeFound;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: FFMPEG RELATED
 * -------------------------------------------------------------- */

// Callback for FFmpeg to call when it has finished with a buffer;
// the opaque pointer should be the sequence number of the frame,
// for debug purposes.
static void avFrameFreeCallback(void *opaque, uint8_t *data)
{
    free(data);
    (void) opaque;
    // W_LOG_DEBUG("video codec is done with frame %llu.", (uint64_t) opaque);
}

// Push a frame of video data onto the queue; data _must_ be a
// malloc()ed buffer and this function _will_ free that malloc()ed
// data, even in a fail case.
static int avFrameQueuePush(uint8_t *data, unsigned int length,
                            unsigned int sequenceNumber,
                            unsigned int width, unsigned int height,
                            unsigned int yStride)
{
    int errorCode = -ENOMEM;

    AVFrame *avFrame = av_frame_alloc();
    if (avFrame) {
        avFrame->format = AV_PIX_FMT_YUV420P;
        avFrame->width = width;
        avFrame->height = height;
        // Each line size is the width of a plane (Y, U or V) plus packing,
        // e.g. in the case of a 960 pixel wide frame the stride is 1024.
        // But in YUV420 only the Y plane is at full resolution, the U and
        // V planes are at half resolution (e.g. 512), hence the divide by
        // two for planes 1 and 2 below
        avFrame->linesize[0] = yStride;
        avFrame->linesize[1] = yStride >> 1;
        avFrame->linesize[2] = yStride >> 1;
        avFrame->time_base = W_VIDEO_STREAM_TIME_BASE_AVRATIONAL;
        avFrame->pts = sequenceNumber;
        avFrame->duration = 1;
        // avFrameFreeCallback() is the function which ultimately frees
        // the video data we are passing around, once the video codec has
        // finished with it
        avFrame->buf[0] = av_buffer_create(data, length,
                                           avFrameFreeCallback,
                                           (void *) (uint64_t) sequenceNumber,
                                           0);
        if (avFrame->buf[0]) {
            errorCode = 0;
        }
        if (errorCode == 0) {
            errorCode =  av_image_fill_pointers(avFrame->data,
                                                AV_PIX_FMT_YUV420P,
                                                avFrame->height,
                                                avFrame->buf[0]->data,
                                                avFrame->linesize);
        }
        if (errorCode >= 0) {
            errorCode = av_frame_make_writable(avFrame);
        }

        if (errorCode == 0) { // TODO
            errorCode = wMsgPush(gMsgQueueVideoEncode, W_MSG_TYPE_AVFRAME_PTR_PTR,
                                 &avFrame, sizeof(avFrame));
        }

        if (errorCode < 0) {
            // This will cause avFrameFreeCallback() to be
            // called and free the data
            av_frame_free(&avFrame);
            W_LOG_ERROR("unable to push frame %d to video queue (%d)!",
                        sequenceNumber, errorCode);
        }
    }

    return errorCode;
}

// Get video from the codec and write it to the output.
static int videoOutput(AVCodecContext *codecContext, AVFormatContext *formatContext)
{
    int errorCode = -ENOMEM;
    unsigned int numReceivedPackets = 0;

    AVPacket *packet = av_packet_alloc();
    if (packet) {
        errorCode = 0;
        // Call avcodec_receive_packet until it returns AVERROR(EAGAIN),
        // meaning it needs to be fed more input
        do {
            errorCode = avcodec_receive_packet(codecContext, packet);
            if (errorCode == 0) {
                numReceivedPackets++;
                packet->time_base = W_VIDEO_STREAM_TIME_BASE_AVRATIONAL;
                errorCode = av_interleaved_write_frame(formatContext, packet);
                // Apparently av_interleave_write_frame() unreferences the
                // packet so we don't need to worry about that
                gVideoStreamFrameOutputCount++;
            }
        } while (errorCode == 0);
        if ((numReceivedPackets > 0) &&
            ((errorCode == AVERROR(EAGAIN)) || (errorCode == AVERROR_EOF))) {
            // That'll do pig, that'll do
            errorCode = 0;
        } else {
            W_LOG_DEBUG("FFmpeg returned error %d (might be because it needs"
                        " more frames to form an output).", errorCode);
        }
        av_packet_free(&packet);
    } else {
         W_LOG_ERROR("unable to allocate packet for FFmpeg encode!");
    }

    return errorCode;
}

// Flush the video output.
static int videoOutputFlush(AVCodecContext *codecContext, AVFormatContext *formatContext)
{
    int errorCode = 0;

    W_LOG_DEBUG("flushing video output.");

    if ((codecContext != nullptr) &&
        (formatContext != nullptr)) {
        // This puts the codec into flush mode
        errorCode = avcodec_send_frame(codecContext, nullptr);
        if (errorCode == 0) {
            errorCode = videoOutput(codecContext, formatContext);
        }
        // In case we want to use the codec again
        avcodec_flush_buffers(codecContext);
    }

    return errorCode;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: VIEW (I.E. THE WATCHDOG'S VIEW) RELATED
 * -------------------------------------------------------------- */

// Determine whether a point is valid or not
static bool pointIsValid(const cv::Point *point)
{
    return point && (*point != W_POINT_INVALID);
}

// Convert a point from our view coordinates to those of an OpenCV
// frame, limiting at the known edges of the frame.
static int viewToFrameAndLimit(const cv::Point *pointView, cv::Point *pointFrame)
{
    int errorCode = -EINVAL;

    if (pointFrame && pointIsValid(pointView)) {
        errorCode = 0;
        // OpenCV has it is origin top-left, x increases in the same
        // direction as us, y the opposite (OpenCV increases going
        // down, we decrease going down)
        pointFrame->x = W_VIEW_ORIGIN_AS_FRAME.x + pointView->x;
        pointFrame->y = -(pointView->y - W_VIEW_ORIGIN_AS_FRAME.y);

        if (pointFrame->x < 0) {
            W_LOG_WARN("viewToFrameAndLimit() x value of frame is"
                       " negative (%d), limiting to zero.",
                       pointFrame->x);
            pointFrame->x = 0;
        } else if (pointFrame->x >= W_CAMERA_STREAM_WIDTH_PIXELS) {
            W_LOG_WARN("viewToFrameAndLimit() x value of frame is"
                       " too large (%d), limiting to %d.",
                       pointFrame->x, W_CAMERA_STREAM_WIDTH_PIXELS - 1);
            pointFrame->x = W_CAMERA_STREAM_WIDTH_PIXELS - 1;
        }
        if (pointFrame->y < 0) {
            W_LOG_WARN("viewToFrameAndLimit() y value of frame is"
                       " negative (%d), limiting to zero.",
                       pointFrame->y);
            pointFrame->y = 0;
        } else if (pointFrame->y >= W_CAMERA_STREAM_HEIGHT_PIXELS) {
            W_LOG_WARN("viewToFrameAndLimit() y value of frame is"
                       " too large (%d), limiting to %d.",
                       pointFrame->y, W_CAMERA_STREAM_HEIGHT_PIXELS - 1);
            pointFrame->y = W_CAMERA_STREAM_HEIGHT_PIXELS - 1;
        }
    }

    return errorCode;
}

// Convert a point from those of an OpenCV frame to our view
// coordinates, limiting at the edges of the view.
static int frameToViewAndLimit(const cv::Point *pointFrame, cv::Point *pointView)
{
    int errorCode = -EINVAL;

    if (pointView && pointIsValid(pointFrame)) {
        errorCode = 0;
       // Our origin is in the centre
        pointView->x = W_FRAME_ORIGIN_AS_VIEW.x + pointFrame->x;
        pointView->y = W_FRAME_ORIGIN_AS_VIEW.y - pointFrame->y;

        if (pointView->x < W_VIEW_LEFT) {
            W_LOG_WARN("frameToViewAndLimit() x value of view is"
                       " too small (%d), limiting to %d.",
                       pointView->x, W_VIEW_LEFT);
            pointView->x = W_VIEW_LEFT;
        } else if (pointView->x > W_VIEW_RIGHT) {
            W_LOG_WARN("frameToViewAndLimit() x value of view is"
                       " too large (%d), limiting to %d.",
                       pointView->x, W_VIEW_RIGHT);
            pointView->x = W_VIEW_RIGHT;
        }
        if (pointView->y < W_VIEW_BOTTOM) {
            W_LOG_WARN("frameToViewAndLimit() y value of view is"
                       " too small (%d), limiting to %d.",
                       pointView->y, W_VIEW_BOTTOM);
            pointView->y = W_VIEW_BOTTOM;
        } else if (pointView->y > W_VIEW_TOP) {
            W_LOG_WARN("frameToViewAndLimit() y value of view is"
                       " too large (%d), limiting to %d.",
                       pointView->y, W_VIEW_TOP);
            pointView->y = W_VIEW_TOP;
        }
    }

    return errorCode;
}

// Get the centre and area of a rectangle, limiting sensibly.
static bool rectGetInfoAndLimit(const cv::Rect *rect, wRectInfo_t *rectInfo)
{
    bool isLimited = false;

    if (rect && rectInfo) {
        rectInfo->areaPixels = rect->width * rect->height;
        rectInfo->centreFrame = {rect->x + (rect->width >> 1),
                                 rect->y + (rect->height >> 1)};
        if (rectInfo->areaPixels > W_CAMERA_STREAM_AREA_PIXELS) {
            W_LOG_WARN("rectGetInfoAndLimit() area is"
                       " too large (%d), limiting to %d.",
                       rectInfo->areaPixels, W_CAMERA_STREAM_AREA_PIXELS);
            rectInfo->areaPixels = W_CAMERA_STREAM_AREA_PIXELS;
            isLimited = true;
        }
        if (rectInfo->centreFrame.x >= W_CAMERA_STREAM_WIDTH_PIXELS) {
            W_LOG_WARN("rectGetInfoAndLimit() x frame coordinate of rectangle"
                       " centre is too large (%d), limiting to %d.",
                       rectInfo->centreFrame.x, W_CAMERA_STREAM_WIDTH_PIXELS - 1);
            rectInfo->centreFrame.x = W_CAMERA_STREAM_WIDTH_PIXELS - 1;
            isLimited = true;
        } else if (rectInfo->centreFrame.x < 0) {
            W_LOG_WARN("rectGetInfoAndLimit() x frame coordinate of rectangle"
                       " is negative (%d), limiting to zero.",
                       rectInfo->centreFrame.x);
            rectInfo->centreFrame.x = 0;
            isLimited = true;
        }
        if (rectInfo->centreFrame.y >= W_CAMERA_STREAM_HEIGHT_PIXELS) {
            W_LOG_WARN("rectGetInfoAndLimit() y frame coordinate of rectangle"
                       " centre is too large (%d), limiting to %d.",
                       rectInfo->centreFrame.y, W_CAMERA_STREAM_HEIGHT_PIXELS - 1);
            rectInfo->centreFrame.y = W_CAMERA_STREAM_HEIGHT_PIXELS - 1;
            isLimited = true;
        } else if (rectInfo->centreFrame.y < 0) {
            W_LOG_WARN("rectGetInfoAndLimit() y frame coordinate of rectangle"
                       " is negative (%d), limiting to zero.",
                       rectInfo->centreFrame.y);
            rectInfo->centreFrame.y = 0;
            isLimited = true;
        }
    }

    return !isLimited;
}

// Sorting function for findFocusFrame().
static bool compareRectInfo(wRectInfo_t rectInfoA,
                            wRectInfo_t rectInfoB) {
    return rectInfoA.areaPixels >= rectInfoB.areaPixels;
}

// Find where the focus should be, in frame coordinates,
// given a set of contours in frame coordinates.  The return
// value is the total area of all rectangles in pixels and,
// since rectangles can overlap, it may be large than the
// area of the frame; think of it as a kind of "magnitude
// of activity" measurement, rather than a literal area.
static int findFocusFrame(const std::vector<std::vector<cv::Point>> contours,
                          cv::Point *pointFrame)
{
    int areaPixels = 0;
    std::vector<wRectInfo_t> rectInfos;

    if (pointFrame) {
        *pointFrame = W_POINT_INVALID;
        if (!contours.empty()) {
            // Create a vector of the size and centre of the rectangles that
            // bound each contour and sort them in descending order of size
            for (auto contour: contours) {
                cv::Rect rect = boundingRect(contour);
                wRectInfo_t rectInfo;
                rectGetInfoAndLimit(&rect, &rectInfo);
                rectInfos.push_back(rectInfo);
            }
            sort(rectInfos.begin(), rectInfos.end(), compareRectInfo);

            // Go through the list; the centre of the first (therefore largest)
            // rectangle is assumed to be the centre of our focus, then each
            // rectangle after that is allowed to influence the centre in
            // proportion to its size (relative to the first one)
            cv::Point pointReferenceFrame = rectInfos.front().centreFrame;
            int areaPixelsReference = rectInfos.front().areaPixels;
            areaPixels = areaPixelsReference;
            *pointFrame = pointReferenceFrame;
            for (auto rectInfo = rectInfos.begin() + 1; rectInfo != rectInfos.end(); rectInfo++) {
                *pointFrame += ((rectInfo->centreFrame - pointReferenceFrame) * rectInfo->areaPixels) / areaPixelsReference;
                areaPixels += rectInfo->areaPixels;
            }
        }
    }

    return areaPixels;
}

// Set a variable of type wPointProtected_t.
static int pointProtectedSet(wPointProtected_t *pointProtected,
                             cv::Point *point)
{
    int errorCode = -EINVAL;
    
    if (pointProtected && point) {
        pointProtected->mutex.lock();
        pointProtected->point = *point;
        errorCode = 0;
        pointProtected->mutex.unlock();
    }

    return errorCode;
}

// Get a variable of type wPointProtected_t.
static cv::Point pointProtectedGet(wPointProtected_t *pointProtected)
{
    cv::Point point = W_POINT_INVALID;

    if (pointProtected) {
        pointProtected->mutex.lock();
        point = pointProtected->point;
        pointProtected->mutex.unlock();
    }

    return point;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyImageBuffer_t (OpenCV)
 * -------------------------------------------------------------- */

// Message handler for wMsgBodyImageBuffer_t.
static void msgHandlerImageBuffer(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyImageBuffer_t *imageBuffer = (wMsgBodyImageBuffer_t *) msgBody;
    cv::Point point;

    // This handler doesn't use any context
    (void) context;

    // Do the OpenCV things.  From the comment on this post:
    // https://stackoverflow.com/questions/44517828/transform-a-yuv420p-qvideoframe-into-grayscale-opencv-mat
    // ...we can bring in just the Y portion of the frame as, effectively,
    // a gray-scale image using CV_8UC1, which can be processed
    // quickly. Note that OpenCV is operating in-place on the
    // data, it does not perform a copy
    cv::Mat frameOpenCvGray(imageBuffer->height, imageBuffer->width, CV_8UC1,
                            imageBuffer->data, imageBuffer->stride);

    // Update the background model: this will cause moving areas to
    // appear as pixels with value 255, stationary areas to appear
    // as pixels with value 0
    gBackgroundSubtractor->apply(frameOpenCvGray, gMaskForeground);

    // Apply thresholding to the foreground mask to remove shadows:
    // anything below the first number becomes zero, anything above
    // the first number becomes the second number
    cv::Mat maskThreshold(imageBuffer->height, imageBuffer->width, CV_8UC1);
    cv::threshold(gMaskForeground, maskThreshold, 25, 255, cv::THRESH_BINARY);
    // Perform erosions and dilations on the mask that will remove
    // any small blobs
    cv::Mat element = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat maskDeblobbed(imageBuffer->height, imageBuffer->width, CV_8UC1);
    cv::morphologyEx(maskThreshold, maskDeblobbed, cv::MORPH_OPEN, element);

    // Find the edges of the moving areas, the ones with pixel value 255
    // in the thresholded/deblobbed mask
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(maskDeblobbed, contours, hierarchy,
                     cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    // Filter the edges to keep just the major ones
    std::vector<std::vector<cv::Point>> largeContours;
    for (auto contour: contours) {
        if (contourArea(contour) > 500) {
            largeContours.push_back(contour);
        }
    }

    // Find the place we should focus on the frame,
    // if there is one
    int areaPixels = findFocusFrame(largeContours, &point);
    if ((areaPixels > 0) && (frameToViewAndLimit(&point, &point) == 0)) {
        // Push the new focus to the control loop
        wMsgBodyFocusChange_t focusChange = {.pointView = point,
                                             .areaPixels = areaPixels};
        wMsgPush(gMsgQueueControl, W_MSG_TYPE_FOCUS_CHANGE,
                 &focusChange, sizeof(focusChange));
    }

#if 1
    // Draw bounding boxes
    for (auto contour: largeContours) {
        cv::Rect rect = boundingRect(contour);
        rectangle(frameOpenCvGray, rect,
                  W_DRAWING_SHADE_MOVING_OBJECTS,
                  W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS);
    }
#else
    // Draw the wiggly edges
    cv::drawContours(frameOpenCvGray, largeContours, 0,
                     W_DRAWING_SHADE_MOVING_OBJECTS,
                     W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS,
                     LINE_8, hierarchy, 0);
#endif

    // Draw the current focus onto the gray OpenCV frame
    point = pointProtectedGet(&gFocusPointView);
    if (viewToFrameAndLimit(&point, &point) == 0) {
        cv::circle(frameOpenCvGray, point,
                   W_DRAWING_RADIUS_FOCUS_CIRCLE,
                   W_DRAWING_SHADE_FOCUS_CIRCLE,
                   W_DRAWING_LINE_THICKNESS_FOCUS_CIRCLE);
    }

    // Finally, write the current local time onto the frame
    // First get the time as a string
    char textBuffer[64];
    time_t rawTime;
    time(&rawTime);
    const auto localTime = localtime(&rawTime);
    // %F %T is pretty much ISO8601 format, Chinese format,
    // descending order of magnitude
    strftime(textBuffer, sizeof(textBuffer), "%F %T", localTime);
    std::string timeString(textBuffer);

    // Create a frame, filled with its ahade (white), of the
    // size of the rectangle we want the time to fit inside
    cv::Mat frameDateTime(W_DRAWING_DATE_TIME_HEIGHT_PIXELS,
                          W_DRAWING_DATE_TIME_WIDTH_PIXELS, CV_8UC1,
                          W_DRAWING_DATE_TIME_REGION_SHADE);
    // Write the text to this frame in its shade (black)
    cv::putText(frameDateTime, timeString,
                cv::Point(W_DRAWING_DATE_TIME_MARGIN_PIXELS_X,
                          W_DRAWING_DATE_TIME_HEIGHT_PIXELS -
                          W_DRAWING_DATE_TIME_MARGIN_PIXELS_Y),
                cv::FONT_HERSHEY_SIMPLEX,
                W_DRAWING_DATE_TIME_FONT_HEIGHT,
                W_DRAWING_DATE_TIME_TEXT_SHADE,
                W_DRAWING_DATE_TIME_TEXT_THICKNESS);
    // Create a rectangle of the same size, positioned on the main image
    cv::Rect dateTimeRegion = cv::Rect(W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_X,
                                       imageBuffer->height - W_DRAWING_DATE_TIME_HEIGHT_PIXELS -
                                       W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_Y,
                                       W_DRAWING_DATE_TIME_WIDTH_PIXELS,
                                       W_DRAWING_DATE_TIME_HEIGHT_PIXELS); 
    // Add frameDateTime to frameOpenCvGray inside dateTimeRegion
    cv::addWeighted(frameOpenCvGray(dateTimeRegion), W_DRAWING_DATE_TIME_ALPHA,
                    frameDateTime, 1 - W_DRAWING_DATE_TIME_ALPHA, 0.0,
                    frameOpenCvGray(dateTimeRegion));

    // Stream the camera frame via FFmpeg: avFrameQueuePush()
    // will free the image data buffer we were passed
    unsigned int queueLength = avFrameQueuePush(imageBuffer->data,
                                                imageBuffer->length,
                                                imageBuffer->sequence,
                                                imageBuffer->width,
                                                imageBuffer->height,
                                                imageBuffer->stride);
    if ((gCameraStreamFrameCount % W_CAMERA_FRAME_RATE_HERTZ == 0) &&
        (queueLength != msgQueuePreviousSizeGet(W_MSG_QUEUE_VIDEO_ENCODE))) {
        // Print the size of the backlog once a second if it has changed
        W_LOG_DEBUG("backlog %d frame(s) on video streaming queue",
                    queueLength);
        msgQueuePreviousSizeSet(W_MSG_QUEUE_VIDEO_ENCODE, queueLength);
    }
}

// Message handler free() function for wMsgBodyImageBuffer_t, used by
// msgQueueClear().
static void msgHandlerImageBufferFree(wMsgBody_t *msgBody, void *context)
{
    // This handler doesn't use any context
    (void) context;

    wMsgBodyImageBuffer_t *imageBuffer = (wMsgBodyImageBuffer_t *) msgBody;
    free(imageBuffer->data);
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER AVFrame (FFmpeg encode)
 * -------------------------------------------------------------- */

// Message handler for AVFrame.
static void msgHandlerAvFrame(wMsgBody_t *msgBody, void *context)
{
    AVFrame **avFrame = (AVFrame **) msgBody;
    wVideoEncodeContext_t *videoEncodeContext = (wVideoEncodeContext_t *) context;
    int32_t errorCode = 0;

    // Procedure from https://ffmpeg.org/doxygen/7.0/group__lavc__encdec.html
    // Ownership of the data in the frame now passes
    // to the video codec and will be free'd by
    // avFrameFreeCallback()
    errorCode = avcodec_send_frame(videoEncodeContext->codecContext, *avFrame);
    if (errorCode == 0) {
        errorCode = videoOutput(videoEncodeContext->codecContext,
                                videoEncodeContext->formatContext);
        // Keep track of timing here, at the end of the 
        // complicated camera/video-frame antics, for debug
        // purposes
        monitorTimingUpdate(&gVideoStreamMonitorTiming);
    } else {
        W_LOG_ERROR("error %d from avcodec_send_frame()!", errorCode);
    }
    // Now we can free the frame
    av_frame_free(avFrame);
    if ((errorCode != 0) && (errorCode != AVERROR(EAGAIN))) {
        W_LOG_ERROR("error %d from FFmpeg!", errorCode);
    }
}

// Message handler free() function for AVFrame, used by
// msgQueueClear().
static void msgHandlerAvFrameFree(wMsgBody_t *msgBody, void *context)
{
    // This handler doesn't use any context
    (void) context;

    AVFrame **avFrame = (AVFrame **) msgBody;
    av_frame_free(avFrame);
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyFocusChange_t
 * -------------------------------------------------------------- */

// Message handler for wMsgBodyFocusChange_t.
static void msgHandlerFocusChange(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyFocusChange_t *focusChange = (wMsgBodyFocusChange_t *) msgBody;

    // This handler doesn't use any context
    (void) context;

    //W_LOG_DEBUG("MSG_FOCUS_CHANGE: x %d, y %d, area %d.",
    //            focusChange->pointView.x, focusChange->pointView.y,
    //            focusChange->areaPixels);
    pointProtectedSet(&gFocusPointView, &(focusChange->pointView));
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LIBCAMERA CALLBACK
 * -------------------------------------------------------------- */

// Handle a requestCompleted event from a camera.
static void requestCompleted(libcamera::Request *request)
{
    cv::Point point;

    if (request->status() != libcamera::Request::RequestCancelled) {
       const std::map<const libcamera::Stream *, libcamera::FrameBuffer *> &buffers = request->buffers();

       for (auto bufferPair : buffers) {
            libcamera::FrameBuffer *buffer = bufferPair.second;
            const libcamera::FrameMetadata &metadata = buffer->metadata();

            // Grab the stream's width, height and stride, all of which
            // is encoded in the buffer's cookie when we associated it
            // with the stream
            unsigned int width;
            unsigned int height;
            unsigned int stride;
            cookieDecode(buffer->cookie(), &width, &height, &stride);

            // From this post: https://forums.raspberrypi.com/viewtopic.php?t=347925,
            // need to create a memory map into the frame buffer for OpenCV or FFmpeg
            // to be able to access it.  Each plane (Y, U and V in our case) has a
            // file descriptor, but in fact they are all the same; the file
            // descriptor is for the *entire* DMA buffer, which includes all of the
            // planes at different offsets; there are three planes: Y, U and V.
            unsigned int dmaBufferLength = buffer->planes()[0].length +
                                           buffer->planes()[1].length +
                                           buffer->planes()[2].length;
            uint8_t *dmaBuffer = static_cast<uint8_t *> (mmap(nullptr, dmaBufferLength,
                                                              PROT_READ | PROT_WRITE, MAP_SHARED,
                                                              buffer->planes()[0].fd.get(), 0));

            if (dmaBuffer != MAP_FAILED) {
                // Pass a copy of the camera frame to the image processing queue
                uint8_t *data = (uint8_t *) malloc(dmaBufferLength);
                if (data) {
                    memcpy(data, dmaBuffer, dmaBufferLength);
                    wMsgBodyImageBuffer_t buffer = {.width = width,
                                                    .height = height,
                                                    .stride = stride,
                                                    .sequence = metadata.sequence,
                                                    .data = data,
                                                    .length = dmaBufferLength};
                    unsigned int queueLength = wMsgPush(gMsgQueueImageProcessing,
                                                        W_MSG_TYPE_IMAGE_BUFFER,
                                                        &buffer, sizeof(buffer));
                    if ((gCameraStreamFrameCount % W_CAMERA_FRAME_RATE_HERTZ == 0) &&
                        (queueLength != msgQueuePreviousSizeGet(W_MSG_QUEUE_IMAGE_PROCESSING))) {
                        // Print the size of the backlog once a second if it has changed
                        W_LOG_DEBUG("backlog %d frame(s) in image processing queue",
                                    queueLength);
                        msgQueuePreviousSizeSet(W_MSG_QUEUE_IMAGE_PROCESSING, queueLength);
                    }
                    gCameraStreamFrameCount++;
                } else {
                    W_LOG_ERROR("unable to allocate %d byte(s) for image buffer,"
                                " a frame has been lost.",
                                dmaBufferLength);
                }
                // Done with the mapping now
                munmap(dmaBuffer, dmaBufferLength);
            } else {
                W_LOG_ERROR("mmap() returned error %d, a frame has been lost.",
                            errno);
            }

            // Re-use the request
            request->reuse(libcamera::Request::ReuseBuffers);
            gCamera->queueRequest(request);
        }
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: HW/MOVEMENT RELATED
 * -------------------------------------------------------------- */

// Run a self test of all of the HW and calibrate the motors.
static int hwInit()
{
    int errorCode = -ENXIO;

    W_LOG_INFO("running HW test.");
    // Create and start a camera manager instance
    std::unique_ptr<libcamera::CameraManager> cm = std::make_unique<libcamera::CameraManager>();
    cm->start();

    // List the available cameras
    for (auto const &camera: cm->cameras()) {
        errorCode = 0;
        W_LOG_INFO("found camera ID %s.", camera->id().c_str());
        W_LOG_DEBUG_START("camera properties:\n");
        auto cameraProperties =  camera->properties();
        auto idMap = cameraProperties.idMap();
        unsigned int x = 0;
        for(auto &controlValue: cameraProperties) {
            auto controlId = idMap->at(controlValue.first);
            if (x > 0) {
                W_LOG_DEBUG_MORE("\n");
            }
            W_LOG_DEBUG_MORE("  %06d [%s]: %s",
                             controlValue.first,
                             controlId->name().c_str(),
                             controlValue.second.toString().c_str());
            x++;
        }
        W_LOG_DEBUG_END;
    }

    cm->stop();

    if (errorCode == 0) {
        errorCode = wMotorInit();
    }

    return errorCode;
}

#endif

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// The entry point.
int main(int argc, char *argv[])
{
    int errorCode = -ENXIO;
    wCommandLineParameters_t commandLineParameters;

#if 0
    wVideoEncodeContext_t videoEncode = {};
    AVStream *avStream = nullptr;
#endif

    // Process the command-line parameters
    if (wCommandLineParse(argc, argv, &commandLineParameters) == 0) {
        wCommandLinePrintChoices(&commandLineParameters);
        // Capture CTRL-C so that we can exit in an organised fashion
        wUtilTerminationCaptureSet();

        // Initialise GPIOs
        errorCode = wGpioInit();
        if (errorCode == 0) {
            // We should now be able to initialise the motors
            errorCode = wMotorInit();;
        }
        if (errorCode == 0) {
            // Initialise messaging
            errorCode = wMsgInit();
        }
        if (errorCode == 0) {
            // Intialise the LEDs
            errorCode = wLedInit();
        }
        while ((errorCode == 0) && wUtilKeepGoing()) {
            errorCode = wLedTest();
        }

#if 0
            // Kick off a control thread
            msgQueueThreadStart(W_MSG_QUEUE_CONTROL, msgFd, nullptr, &controlThread);
            // Create and start a camera manager instance
            std::unique_ptr<libcamera::CameraManager> cm = std::make_unique<libcamera::CameraManager>();
            cm->start();

           // Acquire the first (and probably only) camera
            auto cameras = cm->cameras();
            if (!cameras.empty()) {
                std::string cameraId = cameras[0]->id();
                W_LOG_INFO("acquiring camera %s.", cameraId.c_str());
                gCamera = cm->get(cameraId);
                gCamera->acquire();

                // Configure the camera with the stream
                std::unique_ptr<libcamera::CameraConfiguration> cameraCfg = gCamera->generateConfiguration({W_CAMERA_STREAM_ROLE});
                cameraStreamConfigure(cameraCfg->at(0), W_CAMERA_STREAM_FORMAT,
                                      W_CAMERA_STREAM_WIDTH_PIXELS,
                                      W_CAMERA_STREAM_HEIGHT_PIXELS);

                // Validate and apply the configuration
                if (cameraCfg->validate() != libcamera::CameraConfiguration::Valid) {
                    W_LOG_DEBUG("libcamera will adjust those values.");
                }
                gCamera->configure(cameraCfg.get());

                W_LOG_INFO_START("validated/applied camera configuration: ");
                for (std::size_t x = 0; x < cameraCfg->size(); x++) {
                    if (x > 0) {
                        W_LOG_INFO_MORE(", ");
                    }
                    W_LOG_INFO_MORE("%s", cameraCfg->at(x).toString().c_str());
                    x++;
                }
                W_LOG_INFO_MORE(".");
                W_LOG_INFO_END;

                // Allocate frame buffers
                libcamera::FrameBufferAllocator *allocator = new libcamera::FrameBufferAllocator(gCamera);
                errorCode = 0;
                for (auto cfg = cameraCfg->begin(); (cfg != cameraCfg->end()) && (errorCode == 0); cfg++) {
                    errorCode = allocator->allocate(cfg->stream());
                    if (errorCode >= 0) {
                        W_LOG_DEBUG("allocated %d buffer(s) for stream %s.", errorCode,
                                    cfg->toString().c_str());
                        errorCode = 0;
                    } else {
                        W_LOG_ERROR("unable to allocate frame buffers (error code %d)!.",
                                    errorCode);
                    }
                }
                if (errorCode == 0) {
                    W_LOG_DEBUG("creating requests to the camera using the allocated buffers.");
                    // Create a queue of requests using the allocated buffers
                    std::vector<std::unique_ptr<libcamera::Request>> requests;
                    for (auto cfg: *cameraCfg) {
                        libcamera::Stream *stream = cfg.stream();
                        const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers = allocator->buffers(stream);
                        for (unsigned int x = 0; (x < buffers.size()) && (errorCode == 0); x++) {
                            std::unique_ptr<libcamera::Request> request = gCamera->createRequest();
                            if (request) {
                                const std::unique_ptr<libcamera::FrameBuffer> &buffer = buffers[x];
                                errorCode = request->addBuffer(stream, buffer.get());
                                if (errorCode == 0) {
                                    // Encode the width, height and stride into the cookie of
                                    // the FrameBuffer as we will need that information later
                                    // when converting the FrameBuffer to a form that OpenCV
                                    // and FFmpeg understand
                                    buffer->setCookie(cookieEncode(stream->configuration().size.width,
                                                                   stream->configuration().size.height,
                                                                   stream->configuration().stride));
                                    requests.push_back(std::move(request));
                                } else {
                                    W_LOG_ERROR("can't attach buffer to camera request (error code %d)!",
                                                 errorCode);
                                }
                            } else {
                                errorCode = -ENOMEM;
                                W_LOG_ERROR("unable to create request to camera!");
                            }
                        }
                    }
                    if (errorCode == 0) {
                        errorCode = -ENOMEM;
                        // That's got pretty much all of the libcamera stuff, the camera
                        // setup, done.  Now set up the output stream for video recording
                        // using FFmpeg, format being HLS containing H.264-encoded data.
                        const AVOutputFormat *avOutputFormat = av_guess_format("hls", nullptr, nullptr);
                        avformat_alloc_output_context2(&(videoEncode.formatContext), avOutputFormat,
                                                       nullptr,
                                                       (commandLineParameters.outputDirectory + 
                                                        std::string(W_UTIL_DIR_SEPARATOR) +
                                                        commandLineParameters.outputFileName +
                                                        std::string(W_HLS_PLAYLIST_FILE_EXTENSION)).c_str());
                        if (videoEncode.formatContext) {
                            // Configure the HLS options
                            AVDictionary *hlsOptions = nullptr;
                            // Note: the original example I was following:
                            // https://medium.com/@vladakuc/hls-video-streaming-from-opencv-and-ffmpeg-828ca80b4124
                            // set a load of "segment_*" (e.g. segment_time_delta='1.0', segment_list_flags='cache+live')
                            // options, however, though these exist in a "stream segment muxer" (see libavformat\segment.c)
                            // they don't seem to be at all associated with the HLS stream as configured here
                            // and the original example was including them wrongly (it just added them with
                            // an av_dict_set() individually, whereas in fact they have to be added as
                            // sub-dictionary (a string of key-value pairs separated by a colon) with the
                            // key "hls_segment_options") and so it wouldn't have known they had no effect
                            // as avformat_write_header() ignores unused dictionary entries.  So I've
                            // not included the "segment_*" options here.
                            // Look at the bottom of libavformat\hlsenc.c and libavformat\mpegtsenc.c for the
                            // options that _do_ apply, or pipe "ffmpeg -h full" to file and search for "HLS"
                            // in the output.
                            // Note: we don't apply hls_time, to set the segment size, here; instead we set
                            // the "gop_size" of the codec, which is the distance between key-frames, to
                            // W_HLS_SEGMENT_DURATION_SECONDS and then the HLS muxer picks that up and uses it
                            // as the segment size, which is much better, since it ensures a key-frame at the
                            // start of every segment.
                            if ((av_dict_set(&hlsOptions, "hls_base_url",
                                             std::string(W_HLS_BASE_URL W_UTIL_DIR_SEPARATOR +
                                                         commandLineParameters.outputDirectory +
                                                         W_UTIL_DIR_SEPARATOR).c_str(), 0) == 0) &&
                                (av_dict_set(&hlsOptions, "hls_segment_type", "mpegts", 0) == 0) &&
                                (av_dict_set_int(&hlsOptions, "hls_list_size", W_HLS_LIST_SIZE, 0) == 0) &&
                                (av_dict_set_int(&hlsOptions, "hls_allow_cache", 0, 0) == 0) &&
                                (av_dict_set(&hlsOptions, "hls_flags", "delete_segments+" // Delete segments no longer in .m3u8 file
                                                                       "program_date_time", 0) == 0)) { // Not required but nice to have
                                //  Set up the H264 video output stream over HLS
                                avStream = avformat_new_stream(videoEncode.formatContext, nullptr);
                                if (avStream) {
                                    errorCode = -ENODEV;
                                    const AVCodec *videoOutputCodec = avcodec_find_encoder_by_name("libx264");
                                    if (videoOutputCodec) {
                                        errorCode = -ENOMEM;
                                        videoEncode.codecContext = avcodec_alloc_context3(videoOutputCodec);
                                        if (videoEncode.codecContext) {
                                            W_LOG_DEBUG("video codec capabilities 0x%08x.", videoOutputCodec->capabilities);
                                            videoEncode.codecContext->width = cameraCfg->at(0).size.width;
                                            videoEncode.codecContext->height = cameraCfg->at(0).size.height;
                                            videoEncode.codecContext->time_base = W_VIDEO_STREAM_TIME_BASE_AVRATIONAL;
                                            videoEncode.codecContext->framerate = W_VIDEO_STREAM_FRAME_RATE_AVRATIONAL;
                                            // Make sure we get a key frame every segment, otherwise if the
                                            // HLS client has to seek backwards from the front and can't find
                                            // a key frame it may fail to play the stream
                                            videoEncode.codecContext->gop_size = W_HLS_SEGMENT_DURATION_SECONDS * W_CAMERA_FRAME_RATE_HERTZ;
                                            // From the discussion here:
                                            // https://superuser.com/questions/908280/what-is-the-correct-way-to-fix-keyframes-in-ffmpeg-for-dash/1223359#1223359
                                            // ... the intended effect of setting keyint_min to twice
                                            // the GOP size is that key-frames can still be inserted
                                            // at a scene-cut but they don't become the kind of key-frame
                                            // that would cause a segment to end early; this keeps the rate
                                            // for the HLS protocol nice and steady at W_HLS_SEGMENT_DURATION_SECONDS
                                            videoEncode.codecContext->keyint_min = videoEncode.codecContext->gop_size * 2;
                                            videoEncode.codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
                                            videoEncode.codecContext->codec_id = AV_CODEC_ID_H264;
                                            videoEncode.codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
                                            // This is needed to include the frame duration in the encoded
                                            // output, otherwise the HLS bit of av_interleaved_write_frame()
                                            // will emit a warning that frames having zero duration will mean
                                            // the HLS segment timing is orf
                                            videoEncode.codecContext->flags = AV_CODEC_FLAG_FRAME_DURATION;
                                            AVDictionary *codecOptions = nullptr;
                                            // Note: have to set "tune" to "zerolatency" below for the hls.js HLS
                                            // client to work correctly: if you do not then hls.js will only work
                                            // if it is started at exactly the same time as the served stream is
                                            // first started and, also, without this setting hls.js will never
                                            // regain sync should it fall off the stream.  I have no idea why; it
                                            // took me a week of trial and error with a zillion settings to find
                                            // this out
                                            if ((av_dict_set(&codecOptions, "tune", "zerolatency", 0) == 0) &&
                                                (avcodec_open2(videoEncode.codecContext,
                                                               videoOutputCodec, &codecOptions) == 0) &&
                                                (avcodec_parameters_from_context(avStream->codecpar,
                                                                                 videoEncode.codecContext) == 0) &&
                                                (avformat_write_header(videoEncode.formatContext, &hlsOptions) >= 0)) {
                                                // avformat_write_header() and avcodec_open2() modify
                                                // the options passed to them to be any options that weren't
                                                // found
                                                const AVDictionaryEntry *entry = nullptr;
                                                while ((entry = av_dict_iterate(hlsOptions, entry))) {
                                                    W_LOG_WARN("HLS option \"%s\", or value \"%s\", not found.",
                                                               entry->key, entry->value);
                                                }
                                                while ((entry = av_dict_iterate(codecOptions, entry))) {
                                                    W_LOG_WARN("Codec option \"%s\", or value \"%s\", not found.",
                                                               entry->key, entry->value);
                                                }
                                                // Don't see why this should be necessary (everything in here
                                                // seems to have its own copy of time_base: the AVCodecContext does,
                                                // AVFrame does and apparently AVStream does), but the example:
                                                // https://ffmpeg.org/doxygen/trunk/transcode_8c-example.html
                                                // does it and if you don't do it the output has no timing.
                                                avStream->time_base = videoEncode.codecContext->time_base;
                                                errorCode = 0;
                                            } else {
                                                W_LOG_ERROR("unable to either open video codec or write AV format header!");
                                            }
                                        } else {
                                            W_LOG_ERROR("unable to allocate memory for video output context!");
                                        }
                                    } else {
                                        W_LOG_ERROR("unable to find H.264 codec in FFmpeg!");
                                    }
                                } else {
                                    W_LOG_ERROR("unable to allocate memory for video output stream!");
                                }
                            } else {
                                W_LOG_ERROR("unable to allocate memory for a dictionary entry that configures HLS!");
                            }
                        } else {
                            W_LOG_ERROR("unable to allocate memory for video output context!");
                        }
                        if (errorCode == 0) {
                            errorCode = -ENOMEM;
                            // Now set up the OpenCV background subtractor object
                            gBackgroundSubtractor = cv::createBackgroundSubtractorMOG2();
                            if (gBackgroundSubtractor) {
                                errorCode = 0;
                                // We have not yet set any of the controls for the camera;
                                // the only one we care about here is the frame rate,
                                // so that the settings above match.  There is a minimum
                                // and a maximum, setting both the same fixes the rate.
                                // We create a camera control list and pass it to
                                // the start() method when we start the camera.
                                libcamera::ControlList cameraControls;
                                // Units are microseconds.
                                int64_t frameDurationLimit = 1000000 / W_CAMERA_FRAME_RATE_HERTZ;
                                cameraControls.set(libcamera::controls::FrameDurationLimits,
                                                   libcamera::Span<const std::int64_t, 2>({frameDurationLimit,
                                                                                           frameDurationLimit}));
                                // Attach the requestCompleted() handler
                                // function to its events and start the camera;
                                // everything else happens in the callback function
                                gCamera->requestCompleted.connect(requestCompleted);

                                // Kick off a thread to encode video frames
                                msgQueueThreadStart(W_MSG_QUEUE_VIDEO_ENCODE, msgFd,
                                                    &videoEncode, &videoEncodeThread);
                                // Kick off our image processing thread
                                msgQueueThreadStart(W_MSG_QUEUE_IMAGE_PROCESSING, msgFd,
                                                    nullptr, &imageProcessingThread);
                                // Remove any old files for a clean start
                                system(std::string("rm " +
                                                   commandLineParameters.outputDirectory +
                                                   W_UTIL_DIR_SEPARATOR +
                                                   commandLineParameters.outputFileName +
                                                   W_HLS_PLAYLIST_FILE_EXTENSION +
                                                   W_UTIL_SYSTEM_SILENT).c_str() );
                                system(std::string("rm " +
                                                   commandLineParameters.outputDirectory +
                                                   W_UTIL_DIR_SEPARATOR +
                                                   commandLineParameters.outputFileName +
                                                   "*" W_HLS_SEGMENT_FILE_EXTENSION +
                                                   W_UTIL_SYSTEM_SILENT).c_str());

                                // Make sure the output directory exists
                                system(std::string("mkdir -p " +
                                                   commandLineParameters.outputDirectory).c_str());

                                // Pedal to da metal
                                W_LOG_INFO("starting the camera and queueing requests (CTRL-C to stop).");
                                gCamera->start(&cameraControls);
                                for (std::unique_ptr<libcamera::Request> &request: requests) {
                                    gCamera->queueRequest(request.get());
                                }

                                while (wUtilKeepGoing()) {
                                    testLeds();
                                    //sleep(1);
                                }

                                W_LOG_INFO("CTRL-C received, stopping the camera.");
                                gCamera->stop();
                            } else {
                                W_LOG_ERROR("unable to create background subtractor!");
                            }
                        }
                    }
                }
                for (auto cfg: *cameraCfg) {
                    allocator->free(cfg.stream());
                }
                delete allocator;
                gCamera->release();
                gCamera.reset();
            } else {
                W_LOG_ERROR("no cameras found!");
            }

            // Tidy up
            W_LOG_DEBUG("tidying up.");
            cm->stop();
            // Make sure all threads know we have terminated
            wUtilTerminationSet();
            // Stop the image processing thread and empty its queue
            msgQueueThreadStop(W_MSG_QUEUE_IMAGE_PROCESSING, nullptr,
                               &imageProcessingThread);
            // Stop the video encode thread and empty its queue
            msgQueueThreadStop(W_MSG_QUEUE_VIDEO_ENCODE, &videoEncode,
                               &videoEncodeThread);
            videoOutputFlush(videoEncode.codecContext,
                             videoEncode.formatContext);
            // Free all of the FFmpeg stuff
            if (videoEncode.formatContext) {
                av_write_trailer(videoEncode.formatContext);
            }
            avcodec_free_context(&(videoEncode.codecContext));
            if (videoEncode.formatContext) {
                avio_closep(&(videoEncode.formatContext->pb));
                avformat_free_context(videoEncode.formatContext);
            }
            // Stop the control thread and empty its queue
            msgQueueThreadStop(W_MSG_QUEUE_CONTROL, nullptr, &controlThread);
            // Stop the LED thread, the LED control thread and empty its queue
            msgQueueThreadStop(W_MSG_QUEUE_LED, &ledContext, &ledControlThread);
            ledDeinit(&ledContext);
            // Done with messaging now
            if (msgFd >= 0) {
                close(msgFd);
            }

            // Print a load of diagnostic information
            W_LOG_INFO("%d video frame(s) captured by camera, %d passed to encode (%d%%),"
                       " %d encoded video frame(s)).",
                       gCameraStreamFrameCount, gMsgQueue[W_MSG_QUEUE_VIDEO_ENCODE].count, 
                       gMsgQueue[W_MSG_QUEUE_VIDEO_ENCODE].count * 100 / gCameraStreamFrameCount,
                       gVideoStreamFrameOutputCount);
            W_LOG_INFO("average frame gap (at end of video output) over the last"
                       " %d frames %lld ms (a rate of %lld frames/second), largest"
                       " gap %lld ms.",
                       W_UTIL_ARRAY_COUNT(gVideoStreamMonitorTiming.gap),
                       std::chrono::duration_cast<std::chrono::milliseconds>(gVideoStreamMonitorTiming.average).count(),
                       1000 / std::chrono::duration_cast<std::chrono::milliseconds>(gVideoStreamMonitorTiming.average).count(),
                       std::chrono::duration_cast<std::chrono::milliseconds>(gVideoStreamMonitorTiming.largest).count());
        } else {
            W_LOG_ERROR("HW self test failure!");
        }
#endif

        // Done with the LEDs now
        wLedDeinit();
        // Done with messaging
        wMsgDeinit();
        // Disable the stepper motors
        wMotorDeinit();
        // Give back the GPIOs
        wGpioDeinit();

    } else {
        // Print help about the commad line, including the defaults
        wCommandLinePrintHelp(&commandLineParameters);
    }

    return errorCode;
}

// End of file
