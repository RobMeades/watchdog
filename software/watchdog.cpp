/*
 * Copyright 2024 Rob Meades
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
 * @brief The watchdog application, main().  This should be split into
 * multiple files with proper APIs between the image processing,
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

// The Linux/Posix stuff.
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <gpiod.h>

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

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: MISC
 * -------------------------------------------------------------- */

// Compute the number of elements in an array.
#define W_ARRAY_COUNT(array) (sizeof(array) / sizeof(array[0]))

// Print the duration of an operation for debug purposes.
#define W_PRINT_DURATION(x) auto _t1 = std::chrono::high_resolution_clock::now();  \
                            x;                                                     \
                            auto _t2 = std::chrono::high_resolution_clock::now();  \
                            W_LOG_DEBUG("%d ms to do \"" #x "\".",                 \
                                        std::chrono::duration_cast<std::chrono::milliseconds>(_t2 - _t1))

// The directory separator (we only run this on Linux).
#define W_DIR_SEPARATOR "/"

// The character that means "this directory".
#define W_DIR_THIS "."

// The required appendage to a system command to make it silent
// (on Linux, obviously).
#define W_SYSTEM_SILENT " >>/dev/null 2>>/dev/null"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: STRINGIFY
 * -------------------------------------------------------------- */

// Used only by W_STRINGIFY_QUOTED.
#define W_STRINGIFY_LITERAL(x) #x

// Stringify a macro, so if you have:
//
// #define foo bar
//
// ...W_STRINGIFY_QUOTED(foo) is "bar".
#define W_STRINGIFY_QUOTED(x) W_STRINGIFY_LITERAL(x)

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
# define W_CAMERA_FRAME_RATE_HERTZ 25
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

#ifndef W_DRAWING_DATE_TIME_REGION_OFFSET_X
// Horizontal offset for the date/time box when it is placed into the
// main image: 5 will position it nicely on the left. 
# define W_DRAWING_DATE_TIME_REGION_OFFSET_X 5
#endif

#ifndef W_DRAWING_DATE_TIME_REGION_OFFSET_Y
// Vertical offset for the date/time box when it is placed into the
// main image: 5 will position it nicely at the bottom (i.e. this is
// taken away from the image height when making an OpenCV Point). 
# define W_DRAWING_DATE_TIME_REGION_OFFSET_Y 5
#endif

#ifndef W_DRAWING_DATE_TIME_ALPHA
// Opacity, AKA alpha, of the date/time box on top of the main image,
// range 1 to 0. 0.7 is readable but you can still see the
// underlying image.
# define W_DRAWING_DATE_TIME_ALPHA 0.7
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: IMAGE PROCESSING RELATED
 * -------------------------------------------------------------- */

#ifndef W_IMAGE_PROCESSING_LIST_MAX_ELEMENTS
// The number of elements in the video processing queue: not so
// many of these as the buffers are usually quite large, we just
// need to keep up.
# define W_IMAGE_PROCESSING_LIST_MAX_ELEMENTS 10
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: VIDEO-CODING RELATED
 * -------------------------------------------------------------- */

#ifndef W_AVFRAME_LIST_MAX_ELEMENTS
// The maximum number of elements in the video frame queue.
# define W_AVFRAME_LIST_MAX_ELEMENTS 1000
#endif

// The stream time-base as an AVRational (integer pair, numerator
// then denominator) that FFmpeg understands.
#define W_VIDEO_STREAM_TIME_BASE_AVRATIONAL {1, W_CAMERA_FRAME_RATE_HERTZ}

// The video stream frame rate in units of the video stream time-base.
#define W_VIDEO_STREAM_FRAME_RATE_AVRATIONAL {W_CAMERA_FRAME_RATE_HERTZ, 1}

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: HLS VIDEO OUTPUT SETTINGS
 * -------------------------------------------------------------- */

#ifndef W_HLS_FILE_NAME_ROOT_DEFAULT
// The default root name for our HLS video files (.m3u8 and .ts).
# define W_HLS_FILE_NAME_ROOT_DEFAULT "watchdog"
#endif

#ifndef W_HLS_PLAYLIST_FILE_EXTENSION
// Playlist file extension.
# define W_HLS_PLAYLIST_FILE_EXTENSION ".m3u8"
#endif

#ifndef W_HLS_SEGMENT_FILE_EXTENSION
// Segment file extension.
# define W_HLS_SEGMENT_FILE_EXTENSION ".ts"
#endif

#ifndef W_HLS_OUTPUT_DIRECTORY_DEFAULT
// The default output directory; should not end in a "/".
# define W_HLS_OUTPUT_DIRECTORY_DEFAULT W_DIR_THIS
#endif

#ifndef W_HLS_SEGMENT_DURATION_SECONDS
// The duration of a segment in seconds.
# define W_HLS_SEGMENT_DURATION_SECONDS 2
#endif

#ifndef W_HLS_LIST_SIZE
// The number of segments in the list.
# define W_HLS_LIST_SIZE 15
#endif

#ifndef W_HLS_BASE_URL
// The URL to serve from (must NOT end with a "/").
# define W_HLS_BASE_URL "http://10.10.1.16"
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: CONTROL RELATED
 * -------------------------------------------------------------- */

#ifndef W_CONTROL_MSG_LIST_MAX_ELEMENTS
// The maximum number of elements in the control message queue.
# define W_CONTROL_MSG_LIST_MAX_ELEMENTS 1000
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: GPIO RELATED
 * -------------------------------------------------------------- */

#ifndef W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT
// The GPIO input pin that detects the state of the "look left limit"
// switch as one is standing behind the watchdog, looking out of
// the watchdog's eyes, i.e. it is the limit switch on the _right_
// side of the collar as one is standing behind the watchdog.
# define W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT 0
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT
// The GPIO input pin that detects the state of the "look right limit"
// switch as one is standing behind the watchdog, looking out of
// the watchdog's eyes, i.e. it is the limit switch on the _left_
// side of the collar as one is standing behind the watchdog.
# define W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT 0
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT
// The GPIO input pin detecting the state of the "look down limit", i.e.
// the switch on the front of the watchdog's body.
# define W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT 0
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_UP_LIMIT
// The GPIO input pin detecting the state of the "look up limit", i.e.
// the switch on the rear of the watchdog's body.
# define W_GPIO_PIN_INPUT_LOOK_UP_LIMIT 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_ENABLE
// The GPIO output pin that enables the stepper motor that rotates the
// watchdog's head.
# define W_GPIO_PIN_OUTPUT_ROTATE_ENABLE 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION
// The GPIO output pin that sets the direction of rotation: 1 for clock-wise,
// 0 for anti-clockwise.
# define W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_STEP
// The GPIO output pin that, when pulsed, causes the rotation stepper motor
// to move one step.
# define W_GPIO_PIN_OUTPUT_ROTATE_STEP 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_ENABLE
// The GPIO output pin that enables the stepper motor that lowers and
// raises the watchdog's head.
# define W_GPIO_PIN_OUTPUT_VERTICAL_ENABLE 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION
// The GPIO output pin that sets the direction of vertical motion: 1 for,
// down, 0 for up.
# define W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_STEP
// The GPIO output pin that, when pulsed, causes the vertical stepper motor
// to move one step.
# define W_GPIO_PIN_OUTPUT_VERTICAL_STEP 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_EYE_LEFT
// The GPIO pin driving the LED in the left eye of the watchdog.
# define W_GPIO_PIN_OUTPUT_EYE_LEFT 0
#endif

#ifndef W_GPIO_PIN_OUTPUT_EYE_RIGHT
// The GPIO pin driving the LED in the right eye of the watchdog.
# define W_GPIO_PIN_OUTPUT_EYE_RIGHT 0
#endif

#ifndef W_GPIO_CHIP_NUMBER
// The number of the GPIO chip to use: must be 4 for a Pi 5 as that's
// the chip that is wired to the header pins.
# define W_GPIO_CHIP_NUMBER 4
#endif

#ifndef W_GPIO_CONSUMER_NAME
// A string to identify us as a consumer of a GPIO pin.
# define W_GPIO_CONSUMER_NAME "watchdog"
#endif

#ifndef W_GPIO_DEBOUNCE_THRESHOLD
// The number of times an input pin must have read a consistently
// different level to the current level for us to believe that it
// really has changed state.  With a GPIO tick timer of 1 ms and
// four input pins, that means that the pin must have read the same
// level for a full 12 milliseconds before we believe it.
# define W_GPIO_DEBOUNCE_THRESHOLD 3
#endif

#ifndef W_GPIO_TICK_TIMER_PERIOD_US
// The GPIO tick timer period in microseconds: this is used to
// debounce the input pins, reading one each time, so will
// run around the four inputs once every fourth period, so using
// 1 millisecond here would mean an input line is read every
// 4 milliseconds and so a W_GPIO_DEBOUNCE_THRESHOLD of 3 would
// mean that a GPIO would need to have read a consistent level
// for 12 milliseconds.
# define W_GPIO_TICK_TIMER_PERIOD_US 1000
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: LOGGING RELATED
 * -------------------------------------------------------------- */

#ifndef W_MONITOR_TIMING_LENGTH
// The number of frames to average timing over when monitoring.
# define W_MONITOR_TIMING_LENGTH 1000
#endif

#define W_LOG_TAG "Watchdog"

// ANSI colour codes for printing.
#define W_ANSI_COLOUR_RESET "\u001b[0m"
#define W_ANSI_COLOUR_BRIGHT_WHITE "\u001b[37;1m"
#define W_ANSI_COLOUR_BRIGHT_GREEN "\u001b[32;1m"
#define W_ANSI_COLOUR_BRIGHT_YELLOW "\u001b[33;1m"
#define W_ANSI_COLOUR_BRIGHT_RED "\u001b[31;1m"
#define W_ANSI_COLOUR_BRIGHT_MAGENTA "\u001b[35;1m"

// Prefixes for info, warning and error strings.
#define W_INFO W_ANSI_COLOUR_BRIGHT_GREEN "INFO  " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET
#define W_WARN W_ANSI_COLOUR_BRIGHT_YELLOW "WARN  " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET
#define W_ERROR W_ANSI_COLOUR_BRIGHT_RED "ERROR " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET
#define W_DEBUG W_ANSI_COLOUR_BRIGHT_MAGENTA "DEBUG " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET

// Logging macros: one-call.
#define W_LOG_INFO(...) log(W_LOG_TYPE_INFO, __LINE__, __VA_ARGS__)
#define W_LOG_WARN(...) log(W_LOG_TYPE_WARN, __LINE__, __VA_ARGS__)
#define W_LOG_ERROR(...) log(W_LOG_TYPE_ERROR, __LINE__, __VA_ARGS__)
#define W_LOG_DEBUG(...) log(W_LOG_TYPE_DEBUG, __LINE__, __VA_ARGS__)

// Logging macros: multiple calls.
#define W_LOG_INFO_START(...) logStart(W_LOG_TYPE_INFO, __LINE__, __VA_ARGS__)
#define W_LOG_WARN_START(...) logStart(W_LOG_TYPE_WARN, __LINE__, __VA_ARGS__)
#define W_LOG_ERROR_START(...) logStart(W_LOG_TYPE_ERROR, __LINE__, __VA_ARGS__)
#define W_LOG_DEBUG_START(...) logStart(W_LOG_TYPE_DEBUG, __LINE__, __VA_ARGS__)
#define W_LOG_INFO_MORE(...) logMore(W_LOG_TYPE_INFO, __VA_ARGS__)
#define W_LOG_WARN_MORE(...) logMore(W_LOG_TYPE_WARN, __VA_ARGS__)
#define W_LOG_ERROR_MORE(...) logMore(W_LOG_TYPE_ERROR, __VA_ARGS__)
#define W_LOG_DEBUG_MORE(...) logMore(W_LOG_TYPE_DEBUG, __VA_ARGS__)
#define W_LOG_INFO_END logEnd(W_LOG_TYPE_INFO)
#define W_LOG_WARN_END logEnd(W_LOG_TYPE_WARN)
#define W_LOG_ERROR_END logEnd(W_LOG_TYPE_ERROR)
#define W_LOG_DEBUG_END logEnd(W_LOG_TYPE_DEBUG)

/* ----------------------------------------------------------------
 * TYPES: LOGGING RELATED
 * -------------------------------------------------------------- */

// The types of log print.  Values are important as they are
// used as indexes into arrays.
typedef enum {
    W_LOG_TYPE_INFO = 0,
    W_LOG_TYPE_WARN = 1,
    W_LOG_TYPE_ERROR = 2,
    W_LOG_TYPE_DEBUG = 3
} wLogType_t;

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

// A buffer of data from the camera.
typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    unsigned int sequence;
    uint8_t *data;
    unsigned int length;
} wBuffer_t;

// A point with mutex protection, used for the focus point which
// we need to write from the control thread and read from the
// requestCompleted() callback. Aside from static initialisation,
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
 * TYPES: CONTROL MESSAGING RELATED
 * -------------------------------------------------------------- */

// Message types that can be passed to the control thread, must
// match the entries in the union of message bodies and the
// mapping to message body sizes in gMsgBodySize[].
typedef enum {
    W_MSG_TYPE_NONE,
    W_MSG_TYPE_FOCUS_CHANGE               // wMsgBodyFocusChange_t
} wMsgType_t;

// The message body structure corresponding to W_MSG_TYPE_FOCUS_CHANGE.
typedef struct {
    cv::Point pointView;
    int areaPixels;
} wMsgBodyFocusChange_t;

// Union of message bodies, used in wMsgContainer_t. If you add
// a member here you must add a type for it in wMsgType_t and an
// entry for it in gMsgBodySize[].
typedef union {
    int unused;                           // W_MSG_TYPE_NONE
    wMsgBodyFocusChange_t focusChange;    // W_MSG_TYPE_FOCUS_CHANGE
} wMsgBody_t;

// Structure to pass information to the control thread.
typedef struct {
    wMsgType_t type;
    wMsgBody_t *body;
} wMsgContainer_t;

/* ----------------------------------------------------------------
 * TYPES: GPIO RELATED
 * -------------------------------------------------------------- */

// A GPIO input debouncer.
typedef struct {
    struct gpiod_line *line;
    unsigned int notLevelCount;
} wGpioDebounce_t;

// A GPIO input pin and its state.
typedef struct {
    unsigned int pin;
    unsigned int level;
    wGpioDebounce_t debounce;
} wGpioInput_t;

// A GPIO output pin and the state it should be initialised to.
typedef struct {
    unsigned int pin;
    unsigned int initialLevel;
} wGpioOutput_t;

/* ----------------------------------------------------------------
 * TYPES: COMMAND-LINE RELATED
 * -------------------------------------------------------------- */

// Parameters passed to this program.
typedef struct {
    std::string programName;
    std::string outputDirectory;
    std::string outputFileName;
} wCommandLineParameters_t;

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

// Linked list of image buffers.
static std::list<wBuffer_t> gImageProcessingList;

// Mutex to protect the linked list of image buffers.
static std::mutex gImageProcessingListMutex;

// Remember the size of the image processing list, purely for
// information.
static unsigned int gImageProcessingListSize = 0;

/* ----------------------------------------------------------------
 * VARIABLES: VIDEO RELATED
 * -------------------------------------------------------------- */

// Count of frames passed to the video codec, purely for information.
static unsigned int gVideoStreamFrameInputCount = 0;

// Count of frames received from the video codec, purely for
// information.
static unsigned int gVideoStreamFrameOutputCount = 0;

// Remember the size of the frame list going to video, purely
// for information.
static unsigned int gVideoStreamFrameListSize = 0;

// Keep track of timing on the video stream, purely for information.
static wMonitorTiming_t gVideoStreamMonitorTiming;

// Linked list of video frames, FFmpeg-style.
static std::list<AVFrame *> gAvFrameList;

// Mutex to protect the linked list of FFmpeg-format video frames.
static std::mutex gAvFrameListMutex;

/* ----------------------------------------------------------------
 * VARIABLES: CONTROL RELATED
 * -------------------------------------------------------------- */

// Flag that tells us whether or not we've had a CTRL-C.
volatile sig_atomic_t gTerminated = 0;

// Linked list of messages for the control thread.
static std::list<wMsgContainer_t> gMsgContainerList;

// Mutex to protect the linked list of messages.
static std::mutex gMsgContainerListMutex;

// Array of message body sizes; order should match the
// members of wMsgType_t.
static unsigned int gMsgBodySize[] = {0,  // Not used
                                      sizeof(wMsgBodyFocusChange_t)};

/* ----------------------------------------------------------------
 * VARIABLES: GPIO RELATED
 * -------------------------------------------------------------- */

// Our GPIO chip.
static gpiod_chip *gGpioChip = nullptr;

// Array of GPIO input pins.
static wGpioInput_t gGpioInputPin[] = {{.pin = W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT},
                                       {.pin = W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT},
                                       {.pin = W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT},
                                       {.pin = W_GPIO_PIN_INPUT_LOOK_UP_LIMIT}};

// Array of GPIO output pins with their initial levels.
static wGpioOutput_t gGpioOutputPin[] = {{.pin = W_GPIO_PIN_OUTPUT_ROTATE_ENABLE,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_ROTATE_STEP,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_ENABLE,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_STEP,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_EYE_LEFT,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_EYE_RIGHT,
                                          .initialLevel = 0}};

// Counter of where the GPIO tick handler is in the set of input pins.
static unsigned int gGpioInputPinIndex = 0;

// Monitor the number of times we've read GPIOs, purely for information.
static uint64_t gGpioInputReadCount = 0;

// Monitor the start and stop time of GPIO reading, purely for information
static std::chrono::system_clock::time_point gGpioInputReadStart;
static std::chrono::system_clock::time_point gGpioInputReadStop;

// Remember the number of times the GPIO read thread has not been called
// dead on time, purely for information.
static uint64_t gGpioInputReadSlipCount = 0;

/* ----------------------------------------------------------------
 * VARIABLES: LOGGING RELATED
 * -------------------------------------------------------------- */

// Array of log prefixes for the different log types.
static const char *gLogPrefix[] = {W_INFO, W_WARN, W_ERROR, W_DEBUG};

// Array of log destinations for the different log types.
static FILE *gLogDestination[] = {stdout, stdout, stderr, stdout};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LOGGING/MONITORING RELATED
 * -------------------------------------------------------------- */

// Return the right output stream for a log type.
static FILE *logDestination(wLogType_t type)
{
    FILE *destination = stderr;

    if (type < W_ARRAY_COUNT(gLogDestination)) {
        destination = gLogDestination[type];
    }

    return destination;
}

// Return the prefix for a log type.
static const char *logPrefix(wLogType_t type)
{
    const char *prefix = W_ERROR;

    if (type < W_ARRAY_COUNT(gLogDestination)) {
        prefix = gLogPrefix[type];
    }

    return prefix;
}

// Print the start of a logging message.
template<typename ... Args>
static void logStart(wLogType_t type, unsigned int line, Args ... args)
{
    FILE *destination = logDestination(type);
    const char *prefix = logPrefix(type);
    char buffer[32];
    timeval now;

    gettimeofday(&now, NULL);
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", gmtime(&(now.tv_sec)));

    fprintf(destination, "%s.%06ldZ ", buffer, now.tv_usec);
    fprintf(destination, "%s[%4d]: ", prefix, line);
    fprintf(destination, args...);
}

// Print the middle of a logging message, after logStart()
// has been called and before logEnd() is called.
template<typename ... Args>
static void logMore(wLogType_t type, Args ... args)
{
    FILE *destination = logDestination(type);

    fprintf(destination, args...);
}

// Print the end of a logging message, after logStart()
// or logMore() has been called.
template<typename ... Args>
static void logEnd(wLogType_t type)
{
    FILE *destination = logDestination(type);

    fprintf(destination, "\n");
}

// Print a single-line logging message.
template<typename ... Args>
static void log(wLogType_t type, unsigned int line, Args ... args)
{
    logStart(type, line, args...);
    logEnd(type);
}

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
        if (monitorTiming->numGaps >= W_ARRAY_COUNT(monitorTiming->gap)) {
            monitorTiming->oldestGap = &(monitorTiming->gap[0]);
        }
    } else {
        // The monitoring buffer is full, need to rotate it
        monitorTiming->total -= *monitorTiming->oldestGap;
        *monitorTiming->oldestGap = gap;
        monitorTiming->total += gap;
        monitorTiming->oldestGap++;
        if (monitorTiming->oldestGap >= monitorTiming->gap + W_ARRAY_COUNT(monitorTiming->gap)) {
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

        if (errorCode == 0) {
            gAvFrameListMutex.lock();
            errorCode = -ENOBUFS;
            unsigned int queueLength = gAvFrameList.size();
            if (queueLength < W_AVFRAME_LIST_MAX_ELEMENTS) {
                errorCode = queueLength + 1;
                try {
                    // Ownership of the data in the frame
                    // effectively passes to videoEncodeLoop()
                    gAvFrameList.push_back(avFrame);
                    gVideoStreamFrameInputCount++;
                }
                catch(int x) {
                    errorCode = -x;
                }
            }
            gAvFrameListMutex.unlock();
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

// Try to pop a video frame off the queue.
static int avFrameQueueTryPop(AVFrame **avFrame)
{
    int errorCode = -EAGAIN;

    if (avFrame && gAvFrameListMutex.try_lock()) {
        if (!gAvFrameList.empty()) {
            *avFrame = gAvFrameList.front();
            gAvFrameList.pop_front();
            errorCode = 0;
        }
        gAvFrameListMutex.unlock();
    }

    return errorCode;
}

// Empty the video frame queue.
static void avFrameQueueClear()
{
    AVFrame *avFrame = nullptr;

    gAvFrameListMutex.lock();
    while (avFrameQueueTryPop(&avFrame) == 0) {
        av_frame_free(&avFrame);
    }
    gAvFrameListMutex.unlock();
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

// Video encode task/thread/thing.
static void videoEncodeLoop(AVCodecContext *codecContext, AVFormatContext *formatContext)
{
    int32_t errorCode = 0;
    AVFrame *avFrame = nullptr;

    while (!gTerminated) {
        if (avFrameQueueTryPop(&avFrame) == 0) {
            // Procedure from https://ffmpeg.org/doxygen/7.0/group__lavc__encdec.html
            // Ownership of the data in the frame now passes
            // to the video codec and will be free'd by
            // avFrameFreeCallback()
            errorCode = avcodec_send_frame(codecContext, avFrame);
            if (errorCode == 0) {
                errorCode = videoOutput(codecContext, formatContext);
                // Keep track of timing here, at the end of the 
                // complicated camera/video-frame antics, for debug
                // purposes
                monitorTimingUpdate(&gVideoStreamMonitorTiming);
            } else {
                W_LOG_ERROR("error %d from avcodec_send_frame()!", errorCode);
            }
            // Now we can free the frame
            av_frame_free(&avFrame);
            if ((errorCode != 0) && (errorCode != AVERROR(EAGAIN))) {
                W_LOG_ERROR("error %d from FFmpeg!", errorCode);
            }
        }

        // Let others in
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
 * STATIC FUNCTIONS: GPIO RELATED
 * -------------------------------------------------------------- */

// Return the line for a GPIO pin, opening the chip if necessary.
static struct gpiod_line *gpioLineGet(unsigned int pin)
{
    struct gpiod_line *line = nullptr;

    if (!gGpioChip) {
        gGpioChip = gpiod_chip_open_by_number(W_GPIO_CHIP_NUMBER);
    }
    if (gGpioChip) {
        line = gpiod_chip_get_line(gGpioChip, pin);
    }

    return line;
}

// Release a GPIO pin if it has been taken before.
static void gpioRelease(struct gpiod_line *line)
{
    if (gpiod_line_consumer(line) != nullptr) {
        gpiod_line_release(line);
    }
}

// Check if a GPIO pin has already been configured as an output.
static bool gpioIsOutput(struct gpiod_line *line)
{
    return ((gpiod_line_consumer(line) != nullptr) &&
            (gpiod_line_direction(line) == GPIOD_LINE_DIRECTION_OUTPUT));
}

// Configure a GPIO pin.
static int gpioCfg(unsigned int pin, bool isOutput, unsigned int level = 0)
{
    int errorCode = -EINVAL;
    struct gpiod_line *line = gpioLineGet(pin);

    if (line) {
        gpioRelease(line);
        if (isOutput) {
            errorCode = gpiod_line_request_output(line,
                                                  W_GPIO_CONSUMER_NAME,
                                                  level);
        } else {
            errorCode = gpiod_line_request_input(line,
                                                 W_GPIO_CONSUMER_NAME);
        }
    }

    return errorCode;
}

// Set the state of a GPIO pin.
static int gpioSet(unsigned int pin, unsigned int level)
{
    int errorCode = -EINVAL;
    struct gpiod_line *line = gpioLineGet(pin);

    if (line) {
        if (gpioIsOutput(line)) {
            errorCode = gpiod_line_set_value(line, level);
        } else {
            gpioRelease(line);
            errorCode = gpiod_line_request_output(line,
                                                  W_GPIO_CONSUMER_NAME,
                                                  level);
        }
    }

    return errorCode;
}

// Get the state of a GPIO pin.
static int gpioGet(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;

    struct gpiod_line *line = gpioLineGet(pin);
    if (line) {
        levelOrErrorCode = gpiod_line_get_value(line);
    }

    return levelOrErrorCode;
}

// GPIO task/thread/thing to debounce inputs and provide a stable
// input level in gGpioInputPin[].
//
// Note: it would have been nice to read the GPIOs in a signal handler
// directly but the libgpiod functions are not async-safe (brgl,
// author of libgpiod, confirmed this), hence we use a timer and read
// the GPIOs in this thread, triggered from that timer.  This loop
// should be run at max priority and we monitor whether any timer ticks
// are missed in gGpioInputReadSlipCount.
static void gpioReadLoop(int timerFd)
{
    unsigned int level;
    uint64_t numExpiriesSaved = 0;
    uint64_t numExpiries;
    uint64_t numExpiriesPassed;
    struct pollfd pollFd[1] = {0};
    struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
    sigset_t sigMask;

    pollFd[0].fd = timerFd;
    pollFd[0].events = POLLIN | POLLERR | POLLHUP;
    sigemptyset(&sigMask);
    sigaddset(&sigMask, SIGINT);
    gGpioInputReadStart = std::chrono::system_clock::now();
    while (!gTerminated) {
        // Block waiting for the timer to go off for up to a time,
        // or for CTRL-C to land; when the timer goes off the number
        // of times it has expired is returned in numExpiries
        if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
            (read(timerFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {
            gGpioInputReadCount++;
            numExpiriesPassed = numExpiries - numExpiriesSaved;
            if (numExpiriesPassed > 1) {
                gGpioInputReadSlipCount += numExpiriesPassed - 1;
            }
            // Read the level from the next input pin in the array
            wGpioInput_t *gpioInput = &(gGpioInputPin[gGpioInputPinIndex]);
            level = gpiod_line_get_value(gpioInput->debounce.line);
            if (gpioInput->level != level) {
                // Level is different to the last stable level, increment count
                gpioInput->debounce.notLevelCount++;
                if (gpioInput->debounce.notLevelCount > W_GPIO_DEBOUNCE_THRESHOLD) {
                    // Count is big enough that we're sure, set the level and
                    // reset the count
                    gpioInput->level = level;
                    gpioInput->debounce.notLevelCount = 0;
                }
            } else {
                // Current level is the same as the stable level, zero the change count
                gpioInput->debounce.notLevelCount = 0;
            }

            // Next input pin next time
            gGpioInputPinIndex++;
            if (gGpioInputPinIndex >= W_ARRAY_COUNT(gGpioInputPin)) {
                gGpioInputPinIndex = 0;
            }
            numExpiriesSaved = numExpiries;
        }
    }
    gGpioInputReadStop = std::chrono::system_clock::now();
}

// Initialise the GPIO pins and return the file handle of a timer
// that can be used in gpioInputLoop() to perform debouncing.
static int gpioInit()
{
    int fdOrErrorCode = 0;

    // Configure all of the input pins and
    // get their initial states
    for (size_t x = 0; (x < W_ARRAY_COUNT(gGpioInputPin)) &&
                       (fdOrErrorCode == 0); x++) {
        wGpioInput_t *gpioInput = &(gGpioInputPin[x]);
        fdOrErrorCode = gpioCfg(gpioInput->pin, false);
        if (fdOrErrorCode == 0) {
            gpioInput->level = gpioGet(gpioInput->pin);
            gpioInput->debounce.line = gpioLineGet(gpioInput->pin);
            gpioInput->debounce.notLevelCount = 0;
        } else {
            W_LOG_ERROR("unable to set pin %d as an input!",
                        gpioInput->pin);
        }
    }

    // Configure all of the output pins to their
    // initial states
    for (size_t x = 0; (x < W_ARRAY_COUNT(gGpioOutputPin)) &&
                       (fdOrErrorCode == 0); x++) {
        wGpioOutput_t *gpioOutput = &(gGpioOutputPin[x]);
        fdOrErrorCode = gpioCfg(gpioOutput->pin, true, gpioOutput->initialLevel);
        if (fdOrErrorCode != 0) {
            W_LOG_ERROR("unable to set pin %d as an output and %s!",
                        gpioOutput->pin,
                        gpioOutput->initialLevel ? "high" : "low");
        }
    }

    if (fdOrErrorCode == 0) {
        // Set up a tick to drive gpioInputLoop()
        fdOrErrorCode = timerfd_create(CLOCK_MONOTONIC, 0);
        if (fdOrErrorCode >= 0) {
            struct itimerspec timerSpec = {0};
            timerSpec.it_value.tv_nsec = W_GPIO_TICK_TIMER_PERIOD_US * 1000;
            timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
            if (timerfd_settime(fdOrErrorCode, 0, &timerSpec, nullptr) != 0) {
                close(fdOrErrorCode);
                fdOrErrorCode = -errno;
                W_LOG_ERROR("unable to set signal action or set timer, error code %d.",
                            fdOrErrorCode);
            }
        } else {
            fdOrErrorCode = -errno;
            W_LOG_ERROR("unable to create timer, error code %d.", fdOrErrorCode);
        }
    }

    return fdOrErrorCode;
}

// Deinitialise the GPIO pins.
static void gpioDeinit()
{
    struct gpiod_line *line;

    for (size_t x = 0; x < W_ARRAY_COUNT(gGpioInputPin); x++) {
        line = gpioLineGet(x);
        if (line) {
            gpioRelease(line);
        }
    }
    for (size_t x = 0; x < W_ARRAY_COUNT(gGpioOutputPin); x++) {
        line = gpioLineGet(x);
        if (line) {
            gpioRelease(line);
        }
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: CONTROL THREAD RELATED
 * -------------------------------------------------------------- */

// Push a message onto the control queue.  body is copied so
// it can be passed in any which way (and it is up to the
// caller of controlQueueTryPop() to free any message body).
// The addresses of any members of wMsgBody_t can be passed
// in, cast to wMsgBody_t *.
static int controlQueuePush(wMsgType_t type, wMsgBody_t *body)
{
    int errorCode = -EINVAL;

    if (type < W_ARRAY_COUNT(gMsgBodySize)) {
        errorCode = 0;
        wMsgBody_t *bodyCopy = nullptr;
        if (gMsgBodySize[type] > 0) {
            errorCode = -ENOMEM;
            bodyCopy = (wMsgBody_t *) malloc(gMsgBodySize[type]);
            if (bodyCopy) {
                errorCode = 0;
                memcpy(bodyCopy, body, gMsgBodySize[type]);
            }
        }
        if (errorCode == 0) {
            wMsgContainer_t container = {.type = type,
                                         .body = bodyCopy};
            gMsgContainerListMutex.lock();
            errorCode = -ENOBUFS;
            unsigned int queueLength = gMsgContainerList.size();
            if (queueLength < W_CONTROL_MSG_LIST_MAX_ELEMENTS) {
                errorCode = queueLength + 1;
                try {
                    gMsgContainerList.push_back(container);
                }
                catch(int x) {
                    errorCode = -x;
                }
            }
            gMsgContainerListMutex.unlock();
        }

        if (errorCode < 0) {
            if (bodyCopy) {
                free(bodyCopy);
            }
            W_LOG_ERROR("unable to push message type %d, body length %d,"
                        " to control queue (%d)!",
                        type, gMsgBodySize[type], errorCode);
        }
    }

    return errorCode;
}

// Try to pop a message off the control queue.  If a message
// is returned the caller must call free() on msg->body.
static int controlQueueTryPop(wMsgContainer_t *msg)
{
    int errorCode = -EAGAIN;

    if (msg && gMsgContainerListMutex.try_lock()) {
        if (!gMsgContainerList.empty()) {
            *msg = gMsgContainerList.front();
            gMsgContainerList.pop_front();
            errorCode = 0;
        }
        gMsgContainerListMutex.unlock();
    }

    return errorCode;
}

// Empty the control queue.
static void controlQueueClear()
{
    gMsgContainerListMutex.lock();
    gMsgContainerList.clear();
    gMsgContainerListMutex.unlock();
}

// Message handler for wMsgBodyFocusChange_t.
static void controlMsgHandlerFocusChange(wMsgBodyFocusChange_t *focusChange)
{
    W_LOG_DEBUG("MSG_FOCUS_CHANGE: x %d, y %d, area %d.",
                focusChange->pointView.x, focusChange->pointView.y,
                focusChange->areaPixels);
    pointProtectedSet(&gFocusPointView, &(focusChange->pointView));
}

// Control task/thread/thing.
static void controlLoop()
{
    wMsgContainer_t msg;

    while (!gTerminated) {
        if (controlQueueTryPop(&msg) == 0) {
            switch (msg.type) {
                case W_MSG_TYPE_NONE:
                    break;
                case W_MSG_TYPE_FOCUS_CHANGE:
                    controlMsgHandlerFocusChange(&(msg.body->focusChange));
                    break;
                default:
                    W_LOG_WARN("controlLoop() received unknown"
                               " message type (%d)", msg.type);
                    break;
            }
            // Free the message body now that we're done
            free(msg.body);
        }

        // Let others in
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: IMAGE PROCESSING (MOSTLY OPENCV) RELATED
 * -------------------------------------------------------------- */

// Push a data buffer onto the image processing queue.
static int imageProcessingQueuePush(wBuffer_t *buffer)
{
    int errorCode = -EINVAL;

    if (buffer) {
        gImageProcessingListMutex.lock();
        errorCode = -ENOBUFS;
        unsigned int queueLength = gImageProcessingList.size();
        if (queueLength < W_IMAGE_PROCESSING_LIST_MAX_ELEMENTS) {
            errorCode = queueLength + 1;
            try {
                gImageProcessingList.push_back(*buffer);
            }
            catch(int x) {
                errorCode = -x;
            }
        }
        gImageProcessingListMutex.unlock();
    }

    if (errorCode < 0) {
        W_LOG_ERROR("unable to push image to processing queue (%d)!",
                    errorCode);
    }

    return errorCode;
}

// Try to pop a message off the image processing queue.
static int imageProcessingQueueTryPop(wBuffer_t *buffer)
{
    int errorCode = -EAGAIN;

    if (buffer && gImageProcessingListMutex.try_lock()) {
        if (!gImageProcessingList.empty()) {
            *buffer = gImageProcessingList.front();
            gImageProcessingList.pop_front();
            errorCode = 0;
        }
        gImageProcessingListMutex.unlock();
    }

    return errorCode;
}

// Empty the image processing queue.
static void imageProcessingQueueClear()
{
    wBuffer_t buffer;

    gImageProcessingListMutex.lock();
    while (imageProcessingQueueTryPop(&buffer) == 0) {
        free(buffer.data);
    }
    gImageProcessingListMutex.unlock();
}

// Image processing task/thread/thing.
static void imageProcessingLoop()
{
    wBuffer_t buffer;
    cv::Point point;

    while (!gTerminated) {
        if (imageProcessingQueueTryPop(&buffer) == 0) {
            // Do the OpenCV things.  From the comment on this post:
            // https://stackoverflow.com/questions/44517828/transform-a-yuv420p-qvideoframe-into-grayscale-opencv-mat
            // ...we can bring in just the Y portion of the frame as, effectively,
            // a gray-scale image using CV_8UC1, which can be processed
            // quickly. Note that OpenCV is operating in-place on the
            // data, it does not perform a copy
            cv::Mat frameOpenCvGray(buffer.height, buffer.width, CV_8UC1,
                                    buffer.data, buffer.stride);

            // Update the background model: this will cause moving areas to
            // appear as pixels with value 255, stationary areas to appear
            // as pixels with value 0
            gBackgroundSubtractor->apply(frameOpenCvGray, gMaskForeground);

            // Apply thresholding to the foreground mask to remove shadows:
            // anything below the first number becomes zero, anything above
            // the first number becomes the second number
            cv::Mat maskThreshold(buffer.height, buffer.width, CV_8UC1);
            cv::threshold(gMaskForeground, maskThreshold, 25, 255, cv::THRESH_BINARY);
            // Perform erosions and dilations on the mask that will remove
            // any small blobs
            cv::Mat element = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
            cv::Mat maskDeblobbed(buffer.height, buffer.width, CV_8UC1);
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
                controlQueuePush(W_MSG_TYPE_FOCUS_CHANGE, (wMsgBody_t *) &focusChange);
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
            cv::Rect dateTimeRegion = cv::Rect(W_DRAWING_DATE_TIME_REGION_OFFSET_X,
                                               buffer.height - W_DRAWING_DATE_TIME_HEIGHT_PIXELS -
                                               W_DRAWING_DATE_TIME_REGION_OFFSET_Y,
                                               W_DRAWING_DATE_TIME_WIDTH_PIXELS,
                                               W_DRAWING_DATE_TIME_HEIGHT_PIXELS); 
            // Add frameDateTime to frameOpenCvGray inside dateTimeRegion
            cv::addWeighted(frameOpenCvGray(dateTimeRegion), W_DRAWING_DATE_TIME_ALPHA,
                            frameDateTime, 1 - W_DRAWING_DATE_TIME_ALPHA, 0.0,
                            frameOpenCvGray(dateTimeRegion));

            // Stream the camera frame via FFmpeg: avFrameQueuePush()
            // will free the image data buffer we were passed
            unsigned int queueLength = avFrameQueuePush(buffer.data, buffer.length,
                                                        buffer.sequence,
                                                        buffer.width,
                                                        buffer.height,
                                                        buffer.stride);
            if ((gCameraStreamFrameCount % W_CAMERA_FRAME_RATE_HERTZ == 0) &&
                (queueLength != gVideoStreamFrameListSize)) {
                // Print the size of the backlog once a second if it has changed
                W_LOG_DEBUG("backlog %d frame(s) on video streaming queue",
                            queueLength);
                gVideoStreamFrameListSize = queueLength;
            }
        }

        // Let others in
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
                    wBuffer_t buffer = {.width = width,
                                        .height = height,
                                        .stride = stride,
                                        .sequence = metadata.sequence,
                                        .data = data,
                                        .length = dmaBufferLength};
                    unsigned int queueLength = imageProcessingQueuePush(&buffer);
                    if ((gCameraStreamFrameCount % W_CAMERA_FRAME_RATE_HERTZ == 0) &&
                        (queueLength != gImageProcessingListSize)) {
                        // Print the size of the backlog once a second if it has changed
                        W_LOG_DEBUG("backlog %d frame in image processing queue(s)",
                                    queueLength);
                        gImageProcessingListSize = queueLength;
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
 * STATIC FUNCTIONS: COMMAND LINE STUFF
 * -------------------------------------------------------------- */

// Catch a termination signal.
static void terminateSignalHandler(int signal)
{
    gTerminated = 1;
}

// Given a C string that is assumed to be a path, return the directory
// portion of that as a C++ string.
static std::string getDirectoryPath(const char *path, bool absolute=false)
{
    std::string directoryPath;

    if (path) {
        directoryPath = std::string(path);
        if (absolute && !(directoryPath.find_first_of(W_DIR_SEPARATOR) == 0)) {
            // If we haven't already got an absolute path, make it absolute
            char *currentDirName = get_current_dir_name();
            if (currentDirName) {
                directoryPath = std::string(currentDirName) + W_DIR_SEPARATOR + directoryPath;
                free(currentDirName);
            } else {
                W_LOG_ERROR("unable to get the current directory name");
            }
        }
        // Remove any slash off the end to avoid double-slashing when
        // we concatenate this with something else
        unsigned int length = directoryPath.length();
        if ((length > 0) && (directoryPath.find_last_of(W_DIR_SEPARATOR) == length)) {
            directoryPath = directoryPath.substr(0, length - 1);
        }
    }

    return directoryPath;
}

// Given a C string that is assumed to be a path, return the file name
// portion of that string.
static std::string getFileName(const char *path)
{
    std::string fileName;

    if (path) {
        fileName = std::string(path);
        // Skip past any directory separators
        unsigned int pos = fileName.find_last_of(W_DIR_SEPARATOR);
        unsigned int length = fileName.length();
        if (pos != std::string::npos) {
            if (pos < length) {
                fileName = fileName.substr(pos + 1, length);
            } else {
                // Directory separator at the end, therefore no file name
                fileName.clear();
            }
        }
    }

    return fileName;
}

// Process the command-line parameters.  If this function returns
// an error and parameters is not nullptr, it will populate
// parameters with the defaults.
static int commandLineParse(int argc, char *argv[],
                            wCommandLineParameters_t *parameters)
{
    int errorCode = -EINVAL;
    int x = 0;

    if (parameters) {
        parameters->programName = std::string(W_HLS_FILE_NAME_ROOT_DEFAULT);
        parameters->outputDirectory = std::string(W_HLS_OUTPUT_DIRECTORY_DEFAULT);
        parameters->outputFileName = std::string(W_HLS_FILE_NAME_ROOT_DEFAULT);
        if ((argc > 0) && (argv)) {
            // Find the program name in the first argument
            parameters->programName = getFileName(argv[x]);
            x++;
            // Look for all the command line parameters
            errorCode = 0;
            while (x < argc) {
                errorCode = -EINVAL;
                // Test for output directory option
                if (std::string(argv[x]) == "-d") {
                    x++;
                    if (x < argc) {
                        errorCode = 0;
                        std::string str = getDirectoryPath(argv[x]);
                        if (!str.empty()) {
                            parameters->outputDirectory = str;
                        }
                    }
                // Test for output file name option
                } else if (std::string(argv[x]) == "-f") {
                    x++;
                    if (x < argc) {
                        errorCode = 0;
                        std::string str = std::string(argv[x]);
                        if (!str.empty()) {
                            parameters->outputFileName = str;
                        }
                    }
                }
                x++;
            }
        }
    }

    return errorCode;
}

// Print command-line choices.
static void commandLinePrintChoices(wCommandLineParameters_t *parameters)
{
    std::string programName = W_HLS_FILE_NAME_ROOT_DEFAULT;

    if (parameters && !parameters->programName.empty()) {
        programName = parameters->programName;
    }
    std::cout << programName;
    if (parameters) {
        std::cout << ", putting output files ("
                  << W_HLS_PLAYLIST_FILE_EXTENSION << " and "
                  << W_HLS_SEGMENT_FILE_EXTENSION << ") in ";
        if (parameters->outputDirectory != std::string(W_DIR_THIS)) {
            std::cout << parameters->outputDirectory;
        } else {
            std::cout << "this directory";
        }
        std::cout << ", output files will be named "
                  << parameters->outputFileName;
    }
    std::cout << "." << std::endl;
}

// Print command-line help.
static void commandLinePrintHelp(wCommandLineParameters_t *defaults)
{
    std::string programName = W_HLS_FILE_NAME_ROOT_DEFAULT;

    if (defaults && !defaults->programName.empty()) {
        programName = defaults->programName;
    }
    std::cout << programName << ", options are:" << std::endl;

    std::cout << "  -d <directory path> set directory for streaming output"
              << " (default ";
    if (defaults && (defaults->outputDirectory != std::string(W_DIR_THIS))) {
        std::cout << defaults->outputDirectory;
    } else {
        std::cout << "this directory";
    }
    std::cout << ")." << std::endl;

    std::cout << "  -f <file name> set file name for streaming output ("
              <<  W_HLS_PLAYLIST_FILE_EXTENSION << " and "
              <<  W_HLS_SEGMENT_FILE_EXTENSION << " files)";
    if (defaults && !defaults->outputFileName.empty()) {
        std::cout << " (default " << defaults->outputFileName << ")";
    }
    std::cout << "." << std::endl;
    std::cout << "Note that this program needs to be able to change scheduling";
    std::cout << " priority which requires elevated privileges." << std::endl;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// The entry point.
int main(int argc, char *argv[])
{
    int errorCode = -ENXIO;
    wCommandLineParameters_t commandLineParameters;
    AVFormatContext *avFormatContext = nullptr;
    AVCodecContext *avCodecContext = nullptr;
    AVStream *avStream = nullptr;
    int timerFd = -1;
    std::thread gpioReadThread;
    std::thread controlThread;
    std::thread imageProcessingThread;
    std::thread videoEncodeThread;

    // Process the command-line parameters
    if (commandLineParse(argc, argv, &commandLineParameters) == 0) {
        commandLinePrintChoices(&commandLineParameters);
        // Capture CTRL-C so that we can exit in an organised fashion
        signal(SIGINT, terminateSignalHandler);

        // Create and start a camera manager instance
        std::unique_ptr<libcamera::CameraManager> cm = std::make_unique<libcamera::CameraManager>();
        cm->start();

        // List the available cameras
        for (auto const &camera: cm->cameras()) {
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
                    avformat_alloc_output_context2(&avFormatContext, avOutputFormat,
                                                   nullptr,
                                                   (commandLineParameters.outputDirectory + 
                                                    std::string(W_DIR_SEPARATOR) +
                                                    commandLineParameters.outputFileName +
                                                    std::string(W_HLS_PLAYLIST_FILE_EXTENSION)).c_str());
                    if (avFormatContext) {
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
                                         std::string(W_HLS_BASE_URL W_DIR_SEPARATOR +
                                                     commandLineParameters.outputDirectory +
                                                     W_DIR_SEPARATOR).c_str(), 0) == 0) &&
                            (av_dict_set(&hlsOptions, "hls_segment_type", "mpegts", 0) == 0) &&
                            (av_dict_set_int(&hlsOptions, "hls_list_size", W_HLS_LIST_SIZE, 0) == 0) &&
                            (av_dict_set_int(&hlsOptions, "hls_allow_cache", 0, 0) == 0) &&
                            (av_dict_set(&hlsOptions, "hls_flags", "delete_segments+" // Delete segments no longer in .m3u8 file
                                                                   "program_date_time", 0) == 0)) { // Not required but nice to have
                            //  Set up the H264 video output stream over HLS
                            avStream = avformat_new_stream(avFormatContext, nullptr);
                            if (avStream) {
                                errorCode = -ENODEV;
                                const AVCodec *videoOutputCodec = avcodec_find_encoder_by_name("libx264");
                                if (videoOutputCodec) {
                                    errorCode = -ENOMEM;
                                    avCodecContext = avcodec_alloc_context3(videoOutputCodec);
                                    if (avCodecContext) {
                                        W_LOG_DEBUG("video codec capabilities 0x%08x.", videoOutputCodec->capabilities);
                                        avCodecContext->width = cameraCfg->at(0).size.width;
                                        avCodecContext->height = cameraCfg->at(0).size.height;
                                        avCodecContext->time_base = W_VIDEO_STREAM_TIME_BASE_AVRATIONAL;
                                        avCodecContext->framerate = W_VIDEO_STREAM_FRAME_RATE_AVRATIONAL;
                                        // Make sure we get a key frame every segment, otherwise if the
                                        // HLS client has to seek backwards from the front and can't find
                                        // a key frame it may fail to play the stream
                                        avCodecContext->gop_size = W_HLS_SEGMENT_DURATION_SECONDS * W_CAMERA_FRAME_RATE_HERTZ;
                                        // From the discussion here:
                                        // https://superuser.com/questions/908280/what-is-the-correct-way-to-fix-keyframes-in-ffmpeg-for-dash/1223359#1223359
                                        // ... the intended effect of setting keyint_min to twice
                                        // the GOP size is that key-frames can still be inserted
                                        // at a scene-cut but they don't become the kind of key-frame
                                        // that would cause a segment to end early; this keeps the rate
                                        // for the HLS protocol nice and steady at W_HLS_SEGMENT_DURATION_SECONDS
                                        avCodecContext->keyint_min = avCodecContext->gop_size * 2;
                                        avCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
                                        avCodecContext->codec_id = AV_CODEC_ID_H264;
                                        avCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
                                        // This is needed to include the frame duration in the encoded
                                        // output, otherwise the HLS bit of av_interleaved_write_frame()
                                        // will emit a warning that frames having zero duration will mean
                                        // the HLS segment timing is orf
                                        avCodecContext->flags = AV_CODEC_FLAG_FRAME_DURATION;
                                        AVDictionary *codecOptions = nullptr;
                                        // Note: have to set "tune" to "zerolatency" below for the hls.js HLS
                                        // client to work correctly: if you do not then hls.js will only work
                                        // if it is started at exactly the same time as the served stream is
                                        // first started and, also, without this setting hls.js will never
                                        // regain sync should it fall off the stream.  I have no idea why; it
                                        // took me a week of trial and error with a zillion settings to find
                                        // this out
                                        if ((av_dict_set(&codecOptions, "tune", "zerolatency", 0) == 0) &&
                                            (avcodec_open2(avCodecContext, videoOutputCodec, &codecOptions) == 0) &&
                                            (avcodec_parameters_from_context(avStream->codecpar, avCodecContext) == 0) &&
                                            (avformat_write_header(avFormatContext, &hlsOptions) >= 0)) {
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
                                            avStream->time_base = avCodecContext->time_base;
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
                        // Now set up the OpenCV background subtractor object
                        gBackgroundSubtractor = cv::createBackgroundSubtractorMOG2();
                        if (gBackgroundSubtractor) {
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

                            // Initialise GPIOs and a thread driven by a timer to
                            // monitor them
                            errorCode = gpioInit();
                            if (errorCode >= 0) {
                                timerFd = errorCode;
                                gpioReadThread = std::thread(gpioReadLoop, timerFd);
                                // Set the thread priority for GPIO reads
                                // to maximum as we never want to miss one
                                struct sched_param scheduling;
                                scheduling.sched_priority = sched_get_priority_max(SCHED_FIFO);
                                errorCode = pthread_setschedparam(gpioReadThread.native_handle(),
                                                                  SCHED_FIFO, &scheduling);
                                if (errorCode != 0) {
                                    W_LOG_ERROR("unable to set thread priority for GPIO reads"
                                                " (error %d), maybe need sudo?", errorCode);
                                }
                            } else {
                                W_LOG_ERROR("error %d initialising GPIOs!", errorCode);
                            }

                            if (errorCode == 0) {
                                // Kick off a control thread
                                controlThread = std::thread(controlLoop);

                                // Kick off a thread to encode video frames
                                videoEncodeThread = std::thread{videoEncodeLoop,
                                                                avCodecContext,
                                                                avFormatContext}; 

                                // Kick off our image processing thread
                                imageProcessingThread = std::thread(imageProcessingLoop);

                                // Remove any old files for a clean start
                                system(std::string("rm " +
                                                   commandLineParameters.outputDirectory +
                                                   W_DIR_SEPARATOR +
                                                   commandLineParameters.outputFileName +
                                                   W_HLS_PLAYLIST_FILE_EXTENSION +
                                                   W_SYSTEM_SILENT).c_str() );
                                system(std::string("rm " +
                                                   commandLineParameters.outputDirectory +
                                                   W_DIR_SEPARATOR +
                                                   commandLineParameters.outputFileName +
                                                   "*" W_HLS_SEGMENT_FILE_EXTENSION +
                                                   W_SYSTEM_SILENT).c_str());

                                // Make sure the output directory exists
                                system(std::string("mkdir -p " +
                                                   commandLineParameters.outputDirectory).c_str());

                                // Pedal to da metal
                                W_LOG_INFO("starting the camera and queueing requests (CTRL-C to stop).");
                                gCamera->start(&cameraControls);
                                for (std::unique_ptr<libcamera::Request> &request: requests) {
                                    gCamera->queueRequest(request.get());
                                }

                                while (!gTerminated) {
                                    sleep(1);
                                }

                                W_LOG_INFO("CTRL-C received, stopping the camera.");
                                gCamera->stop();
                            }
                        } else {
                            errorCode = -ENOMEM;
                            W_LOG_ERROR("unable to create background subtractor!");
                        }
                    }
                }
            }

            // Tidy up
            W_LOG_DEBUG("tidying up.");
            // Make sure all threads know we have terminated
            gTerminated = true;
            // Stop the image processing thread
            if (imageProcessingThread.joinable()) {
               imageProcessingThread.join();
            }
            // Stop the video encode thread
            if (videoEncodeThread.joinable()) {
               videoEncodeThread.join();
            }
            videoOutputFlush(avCodecContext, avFormatContext);
            if (avFormatContext) {
                av_write_trailer(avFormatContext);
            }
            avcodec_free_context(&avCodecContext);
            if (avFormatContext) {
                avio_closep(&avFormatContext->pb);
                avformat_free_context(avFormatContext);
            }
            for (auto cfg: *cameraCfg) {
                allocator->free(cfg.stream());
            }
            delete allocator;
            gCamera->release();
            gCamera.reset();
            // These are done last for safety as everything
            // should already have been flushed through
            // anyway above
            imageProcessingQueueClear();
            avFrameQueueClear();
            // Stop the control thread
            if (controlThread.joinable()) {
               controlThread.join();
            }
            controlQueueClear();
            // Stop the GPIO tick timer
            if (timerFd >= 0) {
                if (gpioReadThread.joinable()) {
                   gpioReadThread.join();
                }
                close(timerFd);
            }
            // Give back the GPIOs
            gpioDeinit();
            // Print a load of diagnostic information
            W_LOG_INFO("%d video frame(s) captured by camera, %d passed to encode (%d%%),"
                       " %d encoded video frame(s)).",
                       gCameraStreamFrameCount, gVideoStreamFrameInputCount, 
                       gVideoStreamFrameInputCount * 100 / gCameraStreamFrameCount,
                       gVideoStreamFrameOutputCount);
            W_LOG_INFO("average frame gap (at end of video output) over the last"
                       " %d frames %lld ms (a rate of %lld frames/second), largest"
                       " gap %lld ms.",
                       W_ARRAY_COUNT(gVideoStreamMonitorTiming.gap),
                       std::chrono::duration_cast<std::chrono::milliseconds>(gVideoStreamMonitorTiming.average).count(),
                       1000 / std::chrono::duration_cast<std::chrono::milliseconds>(gVideoStreamMonitorTiming.average).count(),
                       std::chrono::duration_cast<std::chrono::milliseconds>(gVideoStreamMonitorTiming.largest).count());
            uint64_t gpioReadsPerInput = gGpioInputReadCount / W_ARRAY_COUNT(gGpioInputPin);
            W_LOG_INFO("each GPIO input read (and debounced) every %lld ms,"
                       " GPIO input read thread wasn't called on schedule %lld time(s).",
                       (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds> (gGpioInputReadStop - gGpioInputReadStart).count() *
                        W_GPIO_DEBOUNCE_THRESHOLD / gpioReadsPerInput,
                       gGpioInputReadSlipCount);
        } else {
            W_LOG_ERROR("no cameras found!");
        }

        // Tidy up
        cm->stop();
    } else {
        // Print help about the commad line, including the defaults
        commandLinePrintHelp(&commandLineParameters);
    }

    return errorCode;
}

// End of file
