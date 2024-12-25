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

// The Linux/Posix stuff.
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
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
 * COMPILE-TIME MACROS: GPIO RELATED
 * -------------------------------------------------------------- */

#ifndef W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT
// The GPIO input pin that detects the state of the "look left limit"
// switch as one is standing behind the watchdog, looking out of
// the watchdog's eyes, i.e. it is the limit switch on the _right_
// side of the collar as one is standing behind the watchdog
// looking forward.
# define W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT 1
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT
// The GPIO input pin that detects the state of the "look right limit"
// switch as one is standing behind the watchdog, looking out of
// the watchdog's eyes, i.e. it is the limit switch on the _left_
// side of the collar as one is standing behind the watchdog
// looking forward.
# define W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT 2
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT
// The GPIO input pin detecting the state of the "look down limit", i.e.
// the switch on the front of the watchdog's body.
# define W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT 3
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_UP_LIMIT
// The GPIO input pin detecting the state of the "look up limit", i.e.
// the switch on the rear of the watchdog's body.
# define W_GPIO_PIN_INPUT_LOOK_UP_LIMIT 4
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_DISABLE
// The GPIO output pin that enables the stepper motor that rotates the
// watchdog's head.
// NOTE: the pin on the Sparkfun board is labelled "enable" but a logic
// 1 disables, a logic 0 enables, so it might better be called "enable bar"
// or more clearly DISABlE
# define W_GPIO_PIN_OUTPUT_ROTATE_DISABLE 5
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION
// The GPIO output pin that sets the direction of rotation: 1 for clock-wise,
// 0 for anti-clockwise.
# define W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION 6
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_STEP
// The GPIO output pin that, when pulsed, causes the rotation stepper motor
// to move one step.
# define W_GPIO_PIN_OUTPUT_ROTATE_STEP 7
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE
// The GPIO output pin that enables the stepper motor that lowers and
// raises the watchdog's head.
// NOTE: the pin on the Sparkfun board is labelled "enable" but a logic
// 1 disables, a logic 0 enables, so it might better be called "enable bar"
// or more clearly DISABlE
# define W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE 8
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION
// The GPIO output pin that sets the direction of vertical motion: 1 for,
// down, 0 for up.
# define W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION 9
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_STEP
// The GPIO output pin that, when pulsed, causes the vertical stepper motor
// to move one step.
# define W_GPIO_PIN_OUTPUT_VERTICAL_STEP 10
#endif

#ifndef W_GPIO_PIN_OUTPUT_EYE_LEFT
// The GPIO pin driving the LED in the left eye of the watchdog.
# define W_GPIO_PIN_OUTPUT_EYE_LEFT 12
#endif

#ifndef W_GPIO_PIN_OUTPUT_EYE_RIGHT
// The GPIO pin driving the LED in the right eye of the watchdog.
# define W_GPIO_PIN_OUTPUT_EYE_RIGHT 13
#endif

#ifndef W_GPIO_CHIP_NUMBER
// The number of the GPIO chip to use: 0 for a Pi 5's header pins.
# define W_GPIO_CHIP_NUMBER 0
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

#ifndef W_GPIO_PWM_TIMER_PERIOD_US
// The GPIO PWM timer period in microseconds: used to drive PWM
// where, since we don't have a capacitor on the LED, we drive
// at quite a high rate.
# define W_GPIO_PWM_TIMER_PERIOD_US 1000
#endif

#ifndef W_GPIO_PWM_MAX_COUNT
// The number of PWM timer intervals that represents 100%.  With
// a PWM timer period of 1 ms, using 20 here keeps the flicker rate
// down to a non-visible 20 ms.
# define W_GPIO_PWM_MAX_COUNT 20
#endif


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
// The duration of a gap between repeats when an LED is
// flashing morse repeatedly, in milliseconds.
# define W_LED_MORSE_DURATION_GAP_REPEAT_MS 1000
#endif

#ifndef W_LED_TICK_TIMER_PERIOD_MS
// The LED tick timer period in milliseconds.
# define W_LED_TICK_TIMER_PERIOD_MS 20
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: MOVEMENT RELATED
 * -------------------------------------------------------------- */

#ifndef W_MOVEMENT_ROTATE_MAX_STEPS
// A hard-coded safety limit on the range of rotational movement.
# define W_MOVEMENT_ROTATE_MAX_STEPS 600
#endif

#ifndef W_MOVEMENT_VERTICAL_MAX_STEPS
// A hard-coded safety limit on the range of vertical movement.
# define W_MOVEMENT_VERTICAL_MAX_STEPS 600
#endif

#ifndef W_MOVEMENT_ROTATE_DIRECTION_SENSE
// The direction that a "1" on the rotate motor's direction
// pin causes the motor to move: 1 since a 1 on the rotate
// motors direction pin causes it to move towards the maximum,
// which is W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT (otherwise it would
// need to be -1)..
# define W_MOVEMENT_ROTATE_DIRECTION_SENSE 1
#endif

#ifndef W_MOVEMENT_VERTICAL_DIRECTION_SENSE
// The direction that a "1" on the vertical motor's direction
// pin causes the motor to move: 1 since a -1 on the vertical
// motors direction pin causes it to move towards the maximum,
// which is W_GPIO_PIN_INPUT_LOOK_UP_LIMIT (otherwise it would
// need to be -1).
# define W_MOVEMENT_VERTICAL_DIRECTION_SENSE -1
#endif

#ifndef W_MOTOR_DIRECTION_WAIT_MS
// The pause between setting the direction that a step is to
// take and requesting the step.
# define W_MOTOR_DIRECTION_WAIT_MS 1
#endif

#ifndef W_MOTOR_STEP_WAIT_MS
// The pause between setting a step pin output low and raising it
// high again; also the pause between a pin being high and
// letting it drop again.
# define W_MOTOR_STEP_WAIT_MS 1
#endif

#ifndef W_MOTOR_LIMIT_MARGIN_STEPS
// How many steps to stay clear of the limit switches in normal
// operation.
# define W_MOTOR_LIMIT_MARGIN_STEPS 10
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
 * TYPES: TIMEOUT RELATED
 * -------------------------------------------------------------- */

/* Structure to hold a start time, used in time-out calculations.
 */
typedef struct {
    std::chrono::time_point<std::chrono::high_resolution_clock> time;
} wTimeoutStart_t;

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
 * TYPES: LED RELATED
 * -------------------------------------------------------------- */

// Identify the LEDs; order is important, must match the order of
// the LEDs in gGpioPwmPin[].
typedef enum {
    W_LED_LEFT = 0,
    W_LED_RIGHT = 1,
    W_LED_MAX_NUM,
    W_LED_BOTH = W_LED_MAX_NUM
} wLed_t;

// Identify the LED modes.
typedef enum {
    W_LED_MODE_TYPE_CONSTANT,
    W_LED_MODE_TYPE_BREATHE
} wLedModeType_t;

// LED level control.
typedef struct {
    unsigned int targetPercent;
    int changePercent;        // The amount to change by
    uint64_t changeInterval;  // The interval between ticks to make a change
    uint64_t changeStartTick; // The tick at which to begin a level change
} wLedLevel_t;

// Control state for constant mode.
typedef struct {
    wLedLevel_t level;
} wLedModeConstant_t;

// Control state for breathe mode.
typedef struct {
    wLedLevel_t levelHigh;
    wLedLevel_t levelLow;
    unsigned int rateMilliHertz;
    unsigned int offsetLeftToRightTicks;
} wLedModeBreathe_t;

// Union of LED modes.
typedef union {
    wLedModeConstant_t constant;
    wLedModeBreathe_t breathe;
} wLedMode_t;

// Morse overlay.
typedef struct {
    char sequenceStr[W_LED_MORSE_MAX_SIZE]; // Null terminated string, use  empty string to cancel previous
    unsigned int repeat; // Repeat the message this number of times
    unsigned int levelPercent;  // Leave at zero to continue with the current level
    unsigned int durationDotMs; // Leave at zero for default W_LED_MORSE_DURATION_DOT_MS
    unsigned int durationDashMs; // Leave at zero for default W_LED_MORSE_DURATION_DASH_MS
    unsigned int durationGapLetterMs;  // Leave at zero for default W_LED_MORSE_DURATION_GAP_LETTER_MS
    unsigned int durationGapWordMs; // Leave at zero for default W_LED_MORSE_DURATION_GAP_WORD_MS
    unsigned int durationGapRepeatMs; // Leave at zero for default W_LED_MORSE_DURATION_GAP_REPEAT_MS
} wLedOverlayMorse_t;

// Wink overlay.
typedef struct {
    unsigned int durationMs; // Leave at zero for default W_LED_WINK_DURATION_MS
} wLedOverlayWink_t;

// Random blink overlay.
typedef struct {
    unsigned int ratePerMinute;
} wLedOverlayRandomBlink_t;

// Control state for one or more LEDs.
typedef struct {
    wLedModeType_t modeType;
    wLedMode_t mode;
    unsigned int levelPercent;
    uint64_t tickLastChange;
    wLedOverlayMorse_t *morse;
    wLedOverlayWink_t *wink;
    wLedOverlayRandomBlink_t *randomBlink;
} wLedState_t;

// Context that must be maintained between the LED message handlers.
typedef struct {
    int fd;
    std::thread thread;
    std::mutex mutex;
    uint64_t tickNow;
    wLedState_t ledState[W_LED_MAX_NUM];
} wLedContext_t;

// LED control sub-structure used by the W_MSG_TYPE_LED_* messages.
typedef struct {
    wLed_t led;
    int offsetLeftToRightMs; // Used if led is W_LED_BOTH
} wLedApply_t;

/* ----------------------------------------------------------------
 * TYPES: MESSAGE TYPES AND THEIR BODIES
 * -------------------------------------------------------------- */

// Message types that can be passed to a queue, must match the
// entries in the union of message bodies and the mapping to
// message body sizes in gMsgBodySize[].
typedef enum {
    W_MSG_TYPE_NONE,
    W_MSG_TYPE_IMAGE_BUFFER,              // wMsgBodyImageBuffer_t
    W_MSG_TYPE_AVFRAME_PTR_PTR,           // Pointer to an AVFrame * (i.e. an FFMpeg type)
    W_MSG_TYPE_FOCUS_CHANGE,              // wMsgBodyFocusChange_t
    W_MSG_TYPE_LED_MODE_CONSTANT,         // wMsgBodyLedModeConstant_t
    W_MSG_TYPE_LED_MODE_BREATHE,          // wMsgBodyLedModeBreathe_t
    W_MSG_TYPE_LED_OVERLAY_MORSE,         // wMsgBodyLedOverlayMorse_t
    W_MSG_TYPE_LED_OVERLAY_WINK,          // wMsgBodyLedOverlayWink_t
    W_MSG_TYPE_LED_OVERLAY_RANDOM_BLINK,         // wMsgBodyLedOverlayRandomBlink_t
    W_MSG_TYPE_LED_LEVEL_SCALE            // wMsgBodyLedLevelScale_t
} wMsgType_t;

// The message body structure corresponding to W_MSG_TYPE_IMAGE_BUFFER.
typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    unsigned int sequence;
    uint8_t *data;
    unsigned int length;
} wMsgBodyImageBuffer_t;

// AVFrame **, the message structure corresponding to W_MSG_TYPE_AVFRAME_PTR_PTR,
// is defined by FFmpeg.

// The message body structure corresponding to W_MSG_TYPE_FOCUS_CHANGE.
typedef struct {
    cv::Point pointView;
    int areaPixels;
} wMsgBodyFocusChange_t;

// The message body structure corresponding to W_MSG_TYPE_LED_MODE_CONSTANT.
typedef struct {
    wLedApply_t apply;
    unsigned int levelPercent;
    unsigned int rampMs;       // The time to get to levelPercent in milliseconds
} wMsgBodyLedModeConstant_t;

// The message body structure corresponding to W_MSG_TYPE_LED_MODE_BREATHE.
typedef struct {
    wLedApply_t apply;
    unsigned int rateMilliHertz;
    unsigned int levelLowPercent;
    unsigned int levelHighPercent;
} wMsgBodyLedModeBreathe_t;

// The message body structure corresponding to W_MSG_TYPE_LED_OVERLAY_MORSE.
typedef struct {
    wLedApply_t apply;
    wLedOverlayMorse_t morse;
} wMsgBodyLedOverlayMorse_t;

// The message body structure corresponding to W_MSG_TYPE_LED_OVERLAY_WINK.
typedef struct {
    wLedApply_t apply;
    wLedOverlayWink_t wink;
} wMsgBodyLedOverlayWink_t;

// The message body structure corresponding to W_MSG_TYPE_LED_OVERLAY_RANDOM_BLINK.
typedef struct {
    wLedApply_t apply;
    wLedOverlayRandomBlink_t randomBlink;
} wMsgBodyLedOverlayRandomBlink_t;

// The message body structure corresponding to W_MSG_TYPE_LED_LEVEL_SCALE.
typedef struct {
    wLedApply_t apply;
    unsigned int percent; // The percentage value to scale all levels by
    unsigned int rampMs;  // The time to get to the new scale in milliseconds
} wMsgBodyLedLevelScale_t;

// Union of message bodies, used in wMsgContainer_t. If you add
// a member here you must add a type for it in wMsgType_t and an
// entry for it in gMsgBodySize[].
typedef union {
    int unused;                                            // W_MSG_TYPE_NONE
    wMsgBodyImageBuffer_t imageBuffer;                     // W_MSG_QUEUE_IMAGE_PROCESSING
    AVFrame **avFrame;                                     // W_MSG_TYPE_AVFRAME_PTR_PTR
    wMsgBodyFocusChange_t focusChange;                     // W_MSG_TYPE_FOCUS_CHANGE
    wMsgBodyLedModeConstant_t ledModeConstant;             // W_MSG_TYPE_LED_MODE_CONSTANT
    wMsgBodyLedModeBreathe_t ledModeBreathe;               // W_MSG_TYPE_LED_MODE_BREATHE
    wMsgBodyLedOverlayMorse_t ledOverlayMorse;             // W_MSG_TYPE_LED_OVERLAY_MORSE
    wMsgBodyLedOverlayWink_t ledOverlayWink;               // W_MSG_TYPE_LED_OVERLAY_WINK
    wMsgBodyLedOverlayRandomBlink_t ledOverlayRandomBlink; // W_MSG_TYPE_LED_OVERLAY_RANDOM_BLINK
    wMsgBodyLedLevelScale_t ledLevelScale;                 // W_MSG_TYPE_LED_LEVEL_SCALE
} wMsgBody_t;

// Container for a message type and body pair, the thing that is
// actually queued.
typedef struct {
    wMsgType_t type;
    wMsgBody_t *body;
} wMsgContainer_t;

/* ----------------------------------------------------------------
 * TYPES: MESSAGE QUEUES
 * -------------------------------------------------------------- */

// The types of message queues: order must match the order of the
// members of gMsgQueue.
typedef enum {
    W_MSG_QUEUE_CONTROL,
    W_MSG_QUEUE_IMAGE_PROCESSING,
    W_MSG_QUEUE_VIDEO_ENCODE,
    W_MSG_QUEUE_LED
} wMsgQueueType_t;

// Function signature of a message handler.
typedef void (wMsgHandlerFunction_t)(wMsgBody_t *msgBody, void *context);

// A message handler: the message handling function and the message
// type it handles.
typedef struct {
    wMsgType_t msgType;
    wMsgHandlerFunction_t *function;
    // If non-NULL then this will be called to free items _inside_
    // the message body; there is no need to call it to free the
    // message body itself, msgQueueLoop() and msgQueueClear()
    // will do that always.
    wMsgHandlerFunction_t *free;
} wMsgHandler_t;

// Definition of a message queue.
typedef struct {
    const char *name; // Name for queue, must not be longer than pthread_setname_np() allows (e.g. 16 characters)
    std::list<wMsgContainer_t> *containerList; // Pointer to a list of messages in containers
    std::mutex *mutex; // Pointer to a mutex to protect the list
    unsigned int maxSize; // The maximum number of elements that can put on the list
    wMsgHandler_t handler[W_MSG_QUEUE_MAX_NUM_HANDLERS]; // The message handlers for this queue, end with W_MSG_TYPE_NONE/nullptr/nullptr
    unsigned int count; // The number of messages pushed
    unsigned int previousSize; // The last recorded length of the list (for debug, used by the caller)
} wMsgQueue_t;

// Forward declarations required since a message handler may push
// messages to another queue.
static int msgQueuePush(wMsgQueueType_t queueType,
                        wMsgType_t msgType, wMsgBody_t *body);
static unsigned int msgQueuePreviousSizeGet(wMsgQueueType_t queueType);
static void msgQueuePreviousSizeSet(wMsgQueueType_t queueType,
                                    unsigned int previousSize);

/* ----------------------------------------------------------------
 * TYPES: GPIO RELATED
 * -------------------------------------------------------------- */

// Storage for debouncing a GPIO.
typedef struct {
    struct gpiod_line *line;
    unsigned int notLevelCount;
} wGpioDebounce_t;

// The possible bias for a GPIO input; if you change the order
// here then you should change gGpioBiasStr[] to match.
typedef enum {
    W_GPIO_BIAS_NONE,
    W_GPIO_BIAS_PULL_DOWN,
    W_GPIO_BIAS_PULL_UP
} wGpioBias_t;

// A GPIO input pin, its biasing and current state.
typedef struct {
    unsigned int pin;
    const char *name;
    wGpioBias_t bias;
    unsigned int level;
    wGpioDebounce_t debounce;
} wGpioInput_t;

// The possible drive strengths for a GPIO output.
typedef enum {
    W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA = 0,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_4_MA = 1,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_6_MA = 2,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_8_MA = 3,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_10_MA = 4,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_12_MA = 5,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_14_MA = 6,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_16_MA = 7
} wGpioDriveStrength_t;

// A GPIO output pin with required drive strength
// and the state the pin should be initialised to.
typedef struct {
    unsigned int pin;
    const char *name;
    wGpioDriveStrength_t driveStrength;
    unsigned int initialLevel;
} wGpioOutput_t;

// An output pin that is a PWM pin.
typedef struct {
    unsigned int pin;
    // Atomic since it is read from the PWM thread and can be
    // written by anyone
    std::atomic<unsigned int> levelPercent;
    struct gpiod_line *line;
} wGpioPwm_t;

/* ----------------------------------------------------------------
 * TYPES: MOTOR/MOVEMENT RELATED
 * -------------------------------------------------------------- */

// The movement types; values are important as they are the index
// to that motor in gMotor[].  The motors are calibrated in this
// order.
typedef enum {
    W_MOVEMENT_TYPE_VERTICAL = 0,
    W_MOVEMENT_TYPE_ROTATE = 1
} wMovementType_t;

// Where the motor should sit by default, e.g. after calibration;
// if you change the order here then you should change
// gMotorRestPositionStr[] to match.
typedef enum {
    W_MOTOR_REST_POSITION_CENTRE,
    W_MOTOR_REST_POSITION_MAX,
    W_MOTOR_REST_POSITION_MIN
} wMotorRestPosition_t;

// Track the position of a motor.
typedef struct {
    const char *name;
    unsigned int safetyLimit; // Safety limit, must be at least max - min
    int pinDisable; // The pin which when set to 1 disables the motor
    int pinDirection; // The pin which, if set to 1, makes steps * senseDirection positive
    int pinStep;  // The pin which causes the motor to step on a 0 to 1 transition
    int pinMax;   // The pin which, when pulled low, indicates max has been reached
    int pinMin;   // The pin which, when pulled low, indicates min has been reached
    int senseDirection; // 1 if a 1 at pinDirection moves towards max, else -1
    wMotorRestPosition_t restPosition;
    bool calibrated; // Ignore the remaining values if this is false
    int max;      // A calibrated limit
    int min;      // A calibrated limit
    int now;
} wMotor_t;

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
 * VARIABLES: MISC
 * -------------------------------------------------------------- */

// Flag that tells us whether or not we've had a CTRL-C.
volatile sig_atomic_t gTerminated = 0;

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

/* ----------------------------------------------------------------
 * VARIABLES: VIDEO RELATED
 * -------------------------------------------------------------- */

// Count of frames received from the video codec, purely for
// information.
static unsigned int gVideoStreamFrameOutputCount = 0;

// Keep track of timing on the video stream, purely for information.
static wMonitorTiming_t gVideoStreamMonitorTiming;

/* ----------------------------------------------------------------
 * VARIABLES: LED RELATED
 * -------------------------------------------------------------- */

// A table of sine-wave magnitudess for a quarter wave, scaled by 100.
static const int gSinePercent[] = {0,   3,   6,   9,   13,  16,  19, 22,  25,  28,
                                   31,  34,  37,  40,  43,  45,  48, 51,  54,  56,
                                   59,  61,  64,  66,  68,  71,  73, 75,  77,  79,
                                   81,  83,  84,  86,  88,  89,  90, 92,  93,  94,
                                   95,  96,  97,  98,  98,  99,  99, 100, 100, 100,
                                   100};

// Names for each of the LEDs, for debug prints only; order must
// match wLed_t.
static const char *gLedStr[] = {"left", "right", "both"};

/* ----------------------------------------------------------------
 * VARIABLES: MESSAGING RELATED
 * -------------------------------------------------------------- */

// NOTE: there are more messaging related variables below
// the definition of the message handling functions.

// Array of message body sizes; order should match the
// members of wMsgType_t.
static unsigned int gMsgBodySize[] = {0,  // Not used
                                      sizeof(wMsgBodyImageBuffer_t),
                                      sizeof(AVFrame **),
                                      sizeof(wMsgBodyFocusChange_t),
                                      sizeof(wMsgBodyLedModeConstant_t),
                                      sizeof(wMsgBodyLedModeBreathe_t),
                                      sizeof(wMsgBodyLedOverlayMorse_t),
                                      sizeof(wMsgBodyLedOverlayWink_t),
                                      sizeof(wMsgBodyLedOverlayRandomBlink_t),
                                      sizeof(wMsgBodyLedLevelScale_t)};

// Message queue for the control thread, used by gMsgQueue[],
// which is defined below the message handlers.
std::list<wMsgContainer_t> gMsgContainerListControl;

// Mutex to protect the control thread message queue, used by
// gMsgQueue[], which is defined below the message handlers.
std::mutex gMsgMutexContol;

// Message queue for the image processing thread, used by gMsgQueue[],
// which is defined below the message handlers.
std::list<wMsgContainer_t> gMsgContainerListImageProcessing;

// Mutex to protect the image processing thread message queue, used
// by gMsgQueue[], which is defined below the message handlers.
std::mutex gMsgMutexImageProcessing;

// Message queue for the video encode thread, used by gMsgQueue[],
// which is defined below the message handlers.
std::list<wMsgContainer_t> gMsgContainerListVideoEncode;

// Mutex to protect the video encode thread message queue, used
// by gMsgQueue[], which is defined below the message handlers.
std::mutex gMsgMutexVideoEncode;

// Message queue for the LED thread, used by gMsgQueue[], which is
// defined below the message handlers.
std::list<wMsgContainer_t> gMsgContainerListLed;

// Mutex to protect the LED thread message queue, used by gMsgQueue[],
// which is defined below the message handlers.
std::mutex gMsgMutexLed;

/* ----------------------------------------------------------------
 * VARIABLES: GPIO RELATED
 * -------------------------------------------------------------- */

// Our GPIO chip.
static gpiod_chip *gGpioChip = nullptr;

// Array of GPIO input pins.
static wGpioInput_t gGpioInputPin[] = {{.pin = W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT,
                                        .name = "look left limit",
                                        .bias = W_GPIO_BIAS_PULL_UP},
                                       {.pin = W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT,
                                        .name = "look right limit",
                                        .bias = W_GPIO_BIAS_PULL_UP},
                                       {.pin = W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT,
                                        .name = "look down limit",
                                        .bias = W_GPIO_BIAS_PULL_UP},
                                       {.pin = W_GPIO_PIN_INPUT_LOOK_UP_LIMIT,
                                        .name = "look up limit",
                                        .bias = W_GPIO_BIAS_PULL_UP}};

// Array of GPIO output pins with their drive strengths
// and initial levels.
static wGpioOutput_t gGpioOutputPin[] = {{.pin = W_GPIO_PIN_OUTPUT_ROTATE_DISABLE,
                                          .name = "rotate disable",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                          .initialLevel = 1}, // Start disabled
                                         {.pin = W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION,
                                          .name = "rotate direction",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_ROTATE_STEP,
                                          .name = "rotate step",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE,
                                          .name = "vertical disable",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                          .initialLevel = 1}, // Start disabled
                                         {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION,
                                          .name = "vertical direction",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_STEP,
                                          .name = "vertical step",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_EYE_LEFT,
                                          .name = "left eye",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_16_MA,
                                          .initialLevel = 0},
                                         {.pin = W_GPIO_PIN_OUTPUT_EYE_RIGHT,
                                          .name = "right eye",
                                          .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_16_MA,
                                          .initialLevel = 0}};

// Array of PWM output pins (which must also be in gGpioOutputPin[]).
static wGpioPwm_t gGpioPwmPin[] = {{.pin = W_GPIO_PIN_OUTPUT_EYE_LEFT},
                                   {.pin = W_GPIO_PIN_OUTPUT_EYE_RIGHT}};

// Array of names for the bias types, just for printing; must be in the same
// order as wGpioBias_t.
static const char *gGpioBiasStr[] = {"none", "pull down", "pull up"};

// Monitor the number of times we've read GPIOs, purely for information.
static uint64_t gGpioInputReadCount = 0;

// Monitor the start and stop time of GPIO reading, purely for information.
static std::chrono::system_clock::time_point gGpioInputReadStart;
static std::chrono::system_clock::time_point gGpioInputReadStop;

// Remember the number of times the GPIO read thread has not been called
// dead on time, purely for information.
static uint64_t gGpioInputReadSlipCount = 0;

/* ----------------------------------------------------------------
 * VARIABLES: MOTOR RELATED
 * -------------------------------------------------------------- */

// Movement tracking: order must match wMovementType_t.
static wMotor_t gMotor[] = {{.name = "vertical",
                             .safetyLimit = W_MOVEMENT_VERTICAL_MAX_STEPS,
                             .pinDisable = W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE,
                             .pinDirection = W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION,
                             .pinStep = W_GPIO_PIN_OUTPUT_VERTICAL_STEP,
                             .pinMax = W_GPIO_PIN_INPUT_LOOK_UP_LIMIT,
                             .pinMin = W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT,
                             .senseDirection = W_MOVEMENT_VERTICAL_DIRECTION_SENSE,
                             .restPosition = W_MOTOR_REST_POSITION_MAX,
                             .calibrated = false},
                            {.name = "rotate",
                             .safetyLimit = W_MOVEMENT_ROTATE_MAX_STEPS,
                             .pinDisable = W_GPIO_PIN_OUTPUT_ROTATE_DISABLE,
                             .pinDirection = W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION,
                             .pinStep = W_GPIO_PIN_OUTPUT_ROTATE_STEP,
                             .pinMax = W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT,
                             .pinMin = W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT,
                             .senseDirection = W_MOVEMENT_ROTATE_DIRECTION_SENSE,
                             .restPosition = W_MOTOR_REST_POSITION_CENTRE,
                             .calibrated = false}};

// Array of names for the rest positions, just for printing; must be in the
// same order as wMotorRestPosition_t.
static const char *gMotorRestPositionStr[] = {"centre", "max", "min"};

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
 * STATIC FUNCTIONS: TIMEOUT RELATED
 * -------------------------------------------------------------- */

// Initialise a time-out with the current time.
static wTimeoutStart_t timeoutStart()
{
    wTimeoutStart_t startTime;
    startTime.time = std::chrono::high_resolution_clock::now();
    return startTime;
}

// Perform a time-out check in a wrap-safe way.
static bool timeoutExpired(wTimeoutStart_t startTime,
                           std::chrono::nanoseconds duration)
{
    auto nowTime = std::chrono::high_resolution_clock::now();
    auto elapsedTime = nowTime - startTime.time;
    return elapsedTime > duration;
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
            errorCode = msgQueuePush(W_MSG_QUEUE_VIDEO_ENCODE,
                                     W_MSG_TYPE_AVFRAME_PTR_PTR,
                                     (wMsgBody_t *) &avFrame);
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

// Configure a GPIO pin.  level and driveStrength are ignored for
// an input pin, bias is ignored for an output pin.
static int gpioCfg(unsigned int pin, bool isOutput,
                   wGpioBias_t bias = W_GPIO_BIAS_NONE,
                   unsigned int level = 0,
                   wGpioDriveStrength_t driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA)
{
    int errorCode = -EINVAL;
    struct gpiod_line *line = gpioLineGet(pin);

    if (line) {
        gpioRelease(line);
        if (isOutput) {
            errorCode = gpiod_line_request_output(line,
                                                  W_GPIO_CONSUMER_NAME,
                                                  level);
            if (errorCode == 0) {
                // Set the drive strength; from the Raspberry Pi site:
                // https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#gpio-addresses
                // ...one writes 0x5a000000 OR'ed with the drive strength
                // in bits 0, 1 and 2 to address 0x7e10002c for GPIOs 0-27,
                // address 0x7e100030 for GPIOs 28-45 or address 0x7e100034
                // for GPIOs 46-53.  All of the GPIO pins on the Pi header
                // are in the first set, which makes things simple.
                off64_t address = 0x7e10002c;
                // Need to use mmap() to get at the memory location and that requires
                // us to find the start of the memory page our address is within 
                int pageSize = getpagesize();
                // Note: getpagesize() returns a size in bytes
                off64_t baseAddress = (address / pageSize) * pageSize;
                off64_t offset = address - baseAddress;
                //unsigned char *baseAddress = (unsigned char *) (((long int) (address + pageSize)) & (long int) ~(pageSize - 1));
                //long int offset = address - baseAddress;
                int memFd = open("/dev/mem", O_RDWR);
                if (memFd) {
                    unsigned char *addressMapped = static_cast<unsigned char *> (mmap(nullptr, offset + sizeof(int),
                                                                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                                                                      memFd, baseAddress));
                    if (addressMapped) {
                        // Since each address covers a range of pins, make sure
                        // not to lower the drive strength again if a pin in the
                        // same range is being written later
                        int value = *(addressMapped + offset);
                        if ((value & 0x07) < driveStrength) {
                            // Bit positions 3 and 4 have a slew-rate and hysteresis
                            // setting respectively; preserve those when writing
                            // the new drive strength
                            *(addressMapped + offset) = (value & 0x00000018) | 0x5a000000 | driveStrength;
                        }
                        munmap(addressMapped, offset + sizeof(int));
                    }
                    close(memFd);
                } else {
                    W_LOG_ERROR("unable to access memory: do you need sudo?");
                }
            }
        } else {
            int flags = 0;
            switch (bias) {
                case W_GPIO_BIAS_PULL_DOWN:
                    flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;
                    break;
                case W_GPIO_BIAS_PULL_UP:
                    flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
                    break;
                default:
                    break;
            }
            errorCode = gpiod_line_request_input_flags(line,
                                                       W_GPIO_CONSUMER_NAME,
                                                       flags);
        }
    }

    return errorCode;
}

// Get the state of a GPIO pin without any debouncing.
static int gpioGetRaw(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;

    struct gpiod_line *line = gpioLineGet(pin);
    if (line) {
        levelOrErrorCode = gpiod_line_get_value(line);
    }

    return levelOrErrorCode;
}

// Get the state of a GPIO pin after debouncing.
static int gpioGet(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;

    for (unsigned int x = 0; (x < W_ARRAY_COUNT(gGpioInputPin)) &&
                             (levelOrErrorCode < 0); x++) {
        wGpioInput_t *gpioInput = &(gGpioInputPin[x]);
        if (gpioInput->pin == pin) {
            levelOrErrorCode = gpioInput->level;
        }
    }

    return levelOrErrorCode;
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

// Get the entry for a PWM pin in gGpioPwmPin.
static wGpioPwm_t *gpioPwmEntry(unsigned int pin)
{
    wGpioPwm_t *gpioPwm = nullptr;

    for (unsigned int x = 0; (x < W_ARRAY_COUNT(gGpioPwmPin)) &&
                             (gpioPwm == nullptr); x++) {
        if (gGpioPwmPin[x].pin == pin) {
            gpioPwm = &(gGpioPwmPin[x]);
        }
    }

    return gpioPwm;
}

// Get the level of a PWM output pin; the pin must be in
// gGpioPwmPin[].
static int gpioPwmGet(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;
    wGpioPwm_t *gpioPwm = gpioPwmEntry(pin);

    if (gpioPwm) {
        levelOrErrorCode = gpioPwm->levelPercent;
    }

    return levelOrErrorCode;
}

// Set the level of a PWM output pin; the pin must be in
// gGpioPwmPin[].
static int gpioPwmSet(unsigned int pin, unsigned int levelPercent)
{
    int errorCode = -EINVAL;
    wGpioPwm_t *gpioPwm = gpioPwmEntry(pin);

    if (gpioPwm) {
        gpioPwm->levelPercent = levelPercent;
        errorCode = 0;
    }

    return errorCode;
}

// Increment the level of a PWM output pin, returning the new
// level; the pin must be in gGpioPwmPin[].
static int gpioPwmInc(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;
    wGpioPwm_t *gpioPwm = gpioPwmEntry(pin);

    if (gpioPwm) {
        if (gpioPwm->levelPercent < 100) {
            gpioPwm->levelPercent++;
        }
        levelOrErrorCode = gpioPwm->levelPercent;
    }

    return levelOrErrorCode;
}

// Decrement the level of a PWM output pin, returning the new
// level; the pin must be in gGpioPwmPin[].
static int gpioPwmDec(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;
    wGpioPwm_t *gpioPwm = gpioPwmEntry(pin);

    if (gpioPwm) {
        if (gpioPwm->levelPercent > 0) {
            gpioPwm->levelPercent--;
        }
        levelOrErrorCode = gpioPwm->levelPercent;
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
// should be run at max priority; any timer ticks that are missed are
// monitored in gGpioInputReadSlipCount.
static void gpioReadLoop(int timerFd)
{
    unsigned int x = 0;
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
        // Block waiting for the tick timer to go off for up to a time,
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
            wGpioInput_t *gpioInput = &(gGpioInputPin[x]);
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
            x++;
            if (x >= W_ARRAY_COUNT(gGpioInputPin)) {
                x = 0;
            }
            numExpiriesSaved = numExpiries;
        }
    }
    gGpioInputReadStop = std::chrono::system_clock::now();

    W_LOG_DEBUG("GPIO read loop has exited");
}

// Task/thread/thing to drive the PWM output of the pins
// in gGpioPwmPin[].
static void gpioPwmLoop(int pwmFd)
{
    unsigned int pwmCount = 0;
    uint64_t numExpiries;
    struct pollfd pollFd[1] = {0};
    struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
    sigset_t sigMask;
    wGpioPwm_t *gpioPwmPinCopy = (wGpioPwm_t *) malloc(sizeof(gGpioPwmPin));

    if (gpioPwmPinCopy) {
        pollFd[0].fd = pwmFd;
        pollFd[0].events = POLLIN | POLLERR | POLLHUP;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);
        while (!gTerminated) {
            // Block waiting for the PWM timer to go off for up to a time,
            // or for CTRL-C to land
            // Change the level of a PWM pin only at the end of a PWM period
            // to avoid any chance of flicker
            memcpy((void *) gpioPwmPinCopy, gGpioPwmPin, sizeof(gGpioPwmPin));
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(pwmFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {
                // Progress all of the PWM pins
                wGpioPwm_t *gpioPwm = gpioPwmPinCopy;
                for (unsigned int x = 0; x < W_ARRAY_COUNT(gGpioPwmPin); x++) {
                    // If the "percentage" count has passed beyond the value
                    // for this pin, set the output low, otherwise if we're
                    // starting the count again and the percentage is non-zero,
                    // set the output pin high
                    if (pwmCount == 0) {
                        if (gpioPwm->levelPercent > 0) {
                            gpiod_line_set_value(gpioPwm->line, 1);
                        }
                    } else if (pwmCount >= gpioPwm->levelPercent * W_GPIO_PWM_MAX_COUNT / 100) {
                        gpiod_line_set_value(gpioPwm->line, 0);
                    }
                    gpioPwm++;
                }
                pwmCount++;
                if (pwmCount >= W_GPIO_PWM_MAX_COUNT) {
                    pwmCount = 0;
                    gpioPwm = gpioPwmPinCopy;
                    // Take a new copy of the pin levels
                    memcpy((void *) gpioPwmPinCopy, gGpioPwmPin, sizeof(gGpioPwmPin));
                }
            }
        }

        // Free memory
        free(gpioPwmPinCopy);
    } else {
        W_LOG_ERROR("unable to allocate %d byte(s) for gpioPwmPin!");
    }

    W_LOG_DEBUG("GPIO PWM loop has exited.");
}

// Initialise the GPIO pins and return the file handle of a timer
// that can be used in gpioInputLoop() to perform debouncing.
static int gpioInit(int *fdPwm)
{
    int fdOrErrorCode = 0;

    // Configure all of the input pins and get their initial states
    for (unsigned int x = 0; (x < W_ARRAY_COUNT(gGpioInputPin)) &&
                             (fdOrErrorCode == 0); x++) {
        wGpioInput_t *gpioInput = &(gGpioInputPin[x]);
        fdOrErrorCode = gpioCfg(gpioInput->pin, false, gpioInput->bias);
        if (fdOrErrorCode == 0) {
            gpioInput->level = gpioGetRaw(gpioInput->pin);
            gpioInput->debounce.line = gpioLineGet(gpioInput->pin);
            gpioInput->debounce.notLevelCount = 0;
        } else {
            W_LOG_ERROR("unable to set pin %d as an input with bias %s!",
                        gpioInput->pin,
                        gGpioBiasStr[gpioInput->bias]);
        }
    }

    // Configure all of the output pins to their initial states
    for (unsigned int x = 0; (x < W_ARRAY_COUNT(gGpioOutputPin)) &&
                             (fdOrErrorCode == 0); x++) {
        wGpioOutput_t *gpioOutput = &(gGpioOutputPin[x]);
        fdOrErrorCode = gpioCfg(gpioOutput->pin, true,
                                W_GPIO_BIAS_NONE,
                                gpioOutput->initialLevel,
                                gpioOutput->driveStrength);
        if (fdOrErrorCode != 0) {
            W_LOG_ERROR("unable to set pin %d as an output,"
                        " drive strength %d and %s!",
                        gpioOutput->pin,
                        gpioOutput->driveStrength,
                        gpioOutput->initialLevel ? "high" : "low");
        }
    }

    if (fdOrErrorCode == 0) {
        // Set up a tick to drive gpioInputLoop()
        fdOrErrorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (fdOrErrorCode >= 0) {
            struct itimerspec timerSpec = {0};
            timerSpec.it_value.tv_nsec = W_GPIO_TICK_TIMER_PERIOD_US * 1000;
            timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
            if (timerfd_settime(fdOrErrorCode, 0, &timerSpec, nullptr) != 0) {
                close(fdOrErrorCode);
                fdOrErrorCode = -errno;
                W_LOG_ERROR("unable to set GPIO tick timer, error code %d.",
                            fdOrErrorCode);
            }
        } else {
            fdOrErrorCode = -errno;
            W_LOG_ERROR("unable to create GPIO tick timer, error code %d.",
                        fdOrErrorCode);
        }
    }

    if ((fdOrErrorCode >= 0) && (fdPwm)) {
        // Set up a tick to drive PWM
        *fdPwm = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (*fdPwm >= 0) {
            struct itimerspec timerSpec = {0};
            timerSpec.it_value.tv_nsec = W_GPIO_PWM_TIMER_PERIOD_US * 1000;
            timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
            if (timerfd_settime(*fdPwm, 0, &timerSpec, nullptr) == 0) {
                // Now that the timer is set, populate gGpioPwmPin
                for (unsigned int x = 0; x < W_ARRAY_COUNT(gGpioPwmPin); x++) {
                    wGpioPwm_t *gpioPwm = &(gGpioPwmPin[x]);
                    gpioPwm->line = gpioLineGet(gpioPwm->pin);
                    gpioPwm->levelPercent = 0;
                    for (unsigned int y = 0; y < W_ARRAY_COUNT(gGpioOutputPin); y++) {
                        wGpioOutput_t *gpioOutput = &(gGpioOutputPin[y]);
                        if (gpioPwm->pin == gpioOutput->pin) {
                            gpioPwm->levelPercent = gpioOutput->initialLevel * 100;
                            break;
                        }
                    }
                }
            } else {
                close(fdOrErrorCode);
                fdOrErrorCode = -errno;
                W_LOG_ERROR("unable to set GPIO PWM timer, error code %d.",
                            fdOrErrorCode);
            }
        } else {
            close(fdOrErrorCode);
            fdOrErrorCode = -errno;
            W_LOG_ERROR("unable to create PWM timer, error code %d.", fdOrErrorCode);
        }
    }

    return fdOrErrorCode;
}

// Deinitialise the GPIO pins.
static void gpioDeinit()
{
    struct gpiod_line *line;

    for (unsigned int x = 0; x < W_ARRAY_COUNT(gGpioInputPin); x++) {
        line = gpioLineGet(x);
        if (line) {
            gpioRelease(line);
        }
    }
    for (unsigned int x = 0; x < W_ARRAY_COUNT(gGpioOutputPin); x++) {
        line = gpioLineGet(x);
        if (line) {
            gpioRelease(line);
        }
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LED RELATED
 * -------------------------------------------------------------- */

// Move any level changes on.
// IMPORTANT: the LED context must be locked before this is called.
static void ledProgressLevel(wLed_t led, wLedState_t *state,
                             uint64_t tickNow, wLedLevel_t *level)
{
    if (state && level && (led < W_ARRAY_COUNT(gGpioPwmPin)) &&
        (state->levelPercent != level->targetPercent) &&
        (tickNow > level->changeStartTick) &&
        (tickNow - state->tickLastChange > level->changeInterval)) {
        int newLevel = ((int) state->levelPercent) + level->changePercent; 
        if (newLevel > 100) {
            newLevel = 100;
        } else if (newLevel < 0) {
            newLevel = 0;
        }
        state->levelPercent = (unsigned int) newLevel;
        gGpioPwmPin[led].levelPercent = state->levelPercent;
        state->tickLastChange = tickNow;
        if (state->levelPercent == level->targetPercent) {
            // Done
            level->changeInterval = 0;
            level->changePercent = 0;
        }
    }
}

// A loop to drive the dynamic behaviours of the LEDs.
static void ledLoop(wLedContext_t *context)
{
    if (context && context->fd) {
        uint64_t numExpiries;
        struct pollfd pollFd[1] = {0};
        struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
        sigset_t sigMask;

        pollFd[0].fd = context->fd;
        pollFd[0].events = POLLIN | POLLERR | POLLHUP;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);
        while (!gTerminated) {
            // Block waiting for our tick timer to go off or for
            // CTRL-C to land
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(context->fd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries)) &&
                context->mutex.try_lock()) {

                // Update the LED pins
                for (unsigned int x = 0; x < W_ARRAY_COUNT(context->ledState); x++) {
                    wLedState_t *state = &(context->ledState[x]);
                    if (state->morse) {
                        // If we are running a morse sequence, that
                        // takes priority

                        // TODO

                    } else {
                        switch (state->modeType) {
                            case W_LED_MODE_TYPE_CONSTANT:
                            {
                                wLedModeConstant_t *mode = &(state->mode.constant);
                                // Progress any change of level
                                ledProgressLevel((wLed_t) x, state, context->tickNow, &(mode->level));
                            }
                            break;
                            case W_LED_MODE_TYPE_BREATHE:
                            {
                                wLedModeBreathe_t *mode = &(state->mode.breathe);
                                // Progress any change of level
                                ledProgressLevel((wLed_t) x, state, context->tickNow, &(mode->level));
                            }
                            break;
                            default:
                                break;
                        }
                        // Random blink overlays the mode
                        if (state->randomBlink) {

                            // TODO

                        }
                        // Wink the mode overlays
                        if (state->wink) {

                            // TODO

                        }
                    }
                }

                context->tickNow++;
                context->mutex.unlock();
            }
        }
    }

    W_LOG_DEBUG("LED loop has exited");
}

// Convert a time in milliseconds to the number of ticks of the
// LED loop.
static int64_t ledMsToTicks(int64_t milliseconds)
{
    return milliseconds / W_LED_TICK_TIMER_PERIOD_MS;
}

// Return the start tick for an LED change.
static uint64_t ledLevelChangeStartSet(uint64_t tickNow,
                                       wLedApply_t *apply,
                                       wLed_t led)
{
    uint64_t changeStartTick = tickNow;

    if ((apply->led == W_LED_BOTH) &&
        (apply->offsetLeftToRightMs != 0)) {
        // Apply an offset if the incoming message was to set
        // both LEDs.  We don't handle the wrap here as the tick
        // is assumed to be an unsigned int64_t...?
        int64_t offsetTicks = ledMsToTicks(apply->offsetLeftToRightMs);
        if ((offsetTicks > 0) && (led == W_LED_RIGHT)) {
            changeStartTick += offsetTicks;
        } else if ((offsetTicks < 0) && (led == W_LED_LEFT)) {
            changeStartTick += -offsetTicks;
        }
    }

    return changeStartTick;
}

// Return the tick-interval at which a ramped LED level should change;
// the amount to increment at each interval is returned in the last
// parameter.
static uint64_t ledLevelChangeIntervalSet(unsigned int rampMs,
                                          unsigned int targetLevelPercent,
                                          unsigned int nowLevelPercent,
                                          int *changePercent)
{
    int64_t changeInterval = INT64_MAX;
    int levelChangePercent = targetLevelPercent - nowLevelPercent;

    if (levelChangePercent != 0) {
        changeInterval = ledMsToTicks(rampMs);
        W_LOG_DEBUG_MORE("\n\n changeInterval %lld", changeInterval);
        // We have rampMs to change from nowLevelPercent
        // to targetLevelPercent.  changeInterval is currently
        // the interval for an increment of one if targetLevelPercent
        // was one higher than nowLevelPercent; work out the interval
        // given the actual difference, noting that it can be negative
        changeInterval /= levelChangePercent;
        // Make sure we return a positive interval
        if (changeInterval < 0) {
            changeInterval = -changeInterval;
        }
        W_LOG_DEBUG_MORE(", levelChangePercent %d, changeInterval %lld\n\n", levelChangePercent, changeInterval);
        if (changePercent) {
            // Set the change per interval
            *changePercent = levelChangePercent;
            if (changeInterval > 0) {
                *changePercent /= changeInterval;
                if (*changePercent == 0) {
                    // Avoid rounding errors leaving us in limbo
                    *changePercent = 1;
                }
            }
        }
    }

    return (uint64_t) changeInterval;
}

// Initialise LEDs; starts ledLoop().
static int ledInit(wLedContext_t *context)
{
    int errorCode = 0;

    // Set up a tick to drive ledLoop()
    errorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (errorCode >= 0) {
        struct itimerspec timerSpec = {0};
        timerSpec.it_value.tv_nsec = W_LED_TICK_TIMER_PERIOD_MS * 1000000;
        timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
        if (timerfd_settime(errorCode, 0, &timerSpec, nullptr) == 0) {
             context->fd = errorCode;
             errorCode = 0;
             // Start the LED loop
             context->thread = std::thread(ledLoop, context);
        } else {
            close(errorCode);
            errorCode = -errno;
            W_LOG_ERROR("unable to set LED tick timer, error code %d.",
                        errorCode);
        }
    } else {
        errorCode = -errno;
        W_LOG_ERROR("unable to create LED tick timer, error code %d.",
                    errorCode);
    }

    return errorCode;
}

// Deinitialise LEDs; stops ledLoop() and free's resources.
static void ledDeinit(wLedContext_t *context)
{
    if (context) {
        if (context->thread.joinable()) {
            context->thread.join();
        }
        if (context->fd) {
            close(context->fd);
        }
    }
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
        msgQueuePush(W_MSG_QUEUE_CONTROL,
                     W_MSG_TYPE_FOCUS_CHANGE, (wMsgBody_t *) &focusChange);
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
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyLedModeConstant_t
 * -------------------------------------------------------------- */

// Update function called only by msgHandlerLedModeConstant().
// IMPORTANT: the LED context must be locked before this is called.
static void msgHandlerLedModeConstantUpdate(wLed_t led,
                                            wLedState_t *state,
                                            uint64_t tickNow,
                                            wMsgBodyLedModeConstant_t *modeSrc)
{
    state->modeType = W_LED_MODE_TYPE_CONSTANT;
    wLedModeConstant_t *modeDst = &(state->mode.constant);
    modeDst->level.targetPercent = modeSrc->levelPercent;
    modeDst->level.changeStartTick = ledLevelChangeStartSet(tickNow,
                                                            &(modeSrc->apply),
                                                            led);
    modeDst->level.changeInterval = ledLevelChangeIntervalSet(modeSrc->rampMs,
                                                              modeSrc->levelPercent,
                                                              state->levelPercent,
                                                              &(modeDst->level.changePercent));
    // This is W_LOG_DEBUG_MORE since it will be within a sequence
    // of log prints in msgHandlerLedModeConstant()
    W_LOG_DEBUG_MORE(" (so start tick %06lld, interval %lld tick(s),"
                     " change per tick %d%%)",
                     modeDst->level.changeStartTick,
                     modeDst->level.changeInterval,
                     modeDst->level.changePercent);
}

// Message handler for wMsgBodyLedModeConstant_t.
static void msgHandlerLedModeConstant(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyLedModeConstant_t *mode = (wMsgBodyLedModeConstant_t *) msgBody;
    wLedContext_t *ledContext = (wLedContext_t *) context;

    if (ledContext) {
        W_LOG_DEBUG_START("HANDLER [%06lld]: wMsgBodyLedModeConstant_t (LED %d,"
                          " %d%%, ramp %d ms, offset %d ms)", ledContext->tickNow,
                          mode->apply.led, mode->levelPercent, mode->rampMs,
                          mode->apply.offsetLeftToRightMs);
        // Lock the LED context
        ledContext->mutex.lock();
        if (mode->apply.led < W_ARRAY_COUNT(ledContext->ledState)) {
            // We're updating one LED
            wLedState_t *state = &(ledContext->ledState[mode->apply.led]);
                W_LOG_DEBUG_MORE("; %s LED mode %d, level %d%%, last change %06lld",
                                 gLedStr[mode->apply.led], state->modeType,
                                 state->levelPercent, state->tickLastChange);
            msgHandlerLedModeConstantUpdate(mode->apply.led, state, ledContext->tickNow, mode);
        } else {
            // Update both LEDs
            for (size_t x = 0; x < W_ARRAY_COUNT(ledContext->ledState); x++) {
                wLedState_t *state = &(ledContext->ledState[x]);
                W_LOG_DEBUG_MORE("; %s LED mode %d, level %d%%, last change %06lld",
                                 gLedStr[x], state->modeType,
                                 state->levelPercent, state->tickLastChange);
                msgHandlerLedModeConstantUpdate((wLed_t) x, state, ledContext->tickNow, mode);
            }
        }
        W_LOG_DEBUG_MORE(".");
        W_LOG_DEBUG_END;

        // Unlock the context again
        ledContext->mutex.unlock();
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyLedModeBreathe_t
 * -------------------------------------------------------------- */

// Message handler for wMsgBodyLedModeBreathe_t.
static void msgHandlerLedModeBreathe(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyLedModeBreathe_t *modeBreathe = (wMsgBodyLedModeBreathe_t *) msgBody;
    wLedContext_t *ledContext = (wLedContext_t *) context;

    // TODO
    (void) modeBreathe;
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyLedOverlayMorse_t
 * -------------------------------------------------------------- */

// Message handler for wMsgBodyLedOverlayMorse_t.
static void msgHandlerLedOverlayMorse(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyLedOverlayMorse_t *overlayMorse = (wMsgBodyLedOverlayMorse_t *) msgBody;
    wLedContext_t *ledContext = (wLedContext_t *) context;

    // TODO
    (void) overlayMorse;
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyLedOverlayWink_t
 * -------------------------------------------------------------- */

// Message handler for wMsgBodyLedOverlayWink_t.
static void msgHandlerLedOverlayWink(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyLedOverlayWink_t *overlayWink = (wMsgBodyLedOverlayWink_t *) msgBody;
    wLedContext_t *ledContext = (wLedContext_t *) context;

    // TODO
    (void) overlayWink;
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyLedOverlayRandomBlink_t
 * -------------------------------------------------------------- */

// Message handler for wMsgBodyLedOverlayRandomBlink_t.
static void msgHandlerLedOverlayRandomBlink(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyLedOverlayRandomBlink_t *overlayBlink = (wMsgBodyLedOverlayRandomBlink_t *) msgBody;
    wLedContext_t *ledContext = (wLedContext_t *) context;

    // TODO
    (void) overlayBlink;
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wMsgBodyLedLevelScale_t
 * -------------------------------------------------------------- */

// Message handler for wMsgBodyLedLevelScale_t.
static void msgHandlerLedLevelScale(wMsgBody_t *msgBody, void *context)
{
    wMsgBodyLedLevelScale_t *levelScale = (wMsgBodyLedLevelScale_t *) msgBody;
    wLedContext_t *ledContext = (wLedContext_t *) context;

    // TODO
    (void) levelScale;
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * MORE VARIABLES: THE MESSAGE QUEUES WITH THEIR MESSAGE HANDLERS
 * -------------------------------------------------------------- */

// Array of message queues; order must match the members of
// wMsgQueueType_t.
static wMsgQueue_t gMsgQueue[] = {{"control",
                                   &gMsgContainerListControl,
                                   &gMsgMutexContol,
                                   W_MSG_QUEUE_MAX_SIZE_CONTROL,
                                   {{W_MSG_TYPE_FOCUS_CHANGE, msgHandlerFocusChange, nullptr},
                                    {W_MSG_TYPE_NONE, nullptr, nullptr}},
                                   0, 0},
                                  {"image process",
                                   &gMsgContainerListImageProcessing,
                                   &gMsgMutexImageProcessing,
                                   W_MSG_QUEUE_MAX_SIZE_IMAGE_PROCESSING,
                                   {{W_MSG_TYPE_IMAGE_BUFFER, msgHandlerImageBuffer, msgHandlerImageBufferFree},
                                    {W_MSG_TYPE_NONE, nullptr, nullptr}},
                                   0, 0},
                                  {"video encode",
                                   &gMsgContainerListVideoEncode,
                                   &gMsgMutexVideoEncode,
                                   W_MSG_QUEUE_MAX_SIZE_VIDEO_ENCODE,
                                   {{W_MSG_TYPE_AVFRAME_PTR_PTR, msgHandlerAvFrame, msgHandlerAvFrameFree},
                                    {W_MSG_TYPE_NONE, nullptr, nullptr}},
                                   0, 0},
                                  {"LED control",
                                   &gMsgContainerListLed,
                                   &gMsgMutexLed,
                                   W_MSG_QUEUE_MAX_SIZE_LED,
                                   {{W_MSG_TYPE_LED_MODE_CONSTANT, msgHandlerLedModeConstant, nullptr},
                                    {W_MSG_TYPE_LED_MODE_BREATHE, msgHandlerLedModeBreathe, nullptr},
                                    {W_MSG_TYPE_LED_OVERLAY_MORSE, msgHandlerLedOverlayMorse, nullptr},
                                    {W_MSG_TYPE_LED_OVERLAY_WINK, msgHandlerLedOverlayWink, nullptr},
                                    {W_MSG_TYPE_LED_OVERLAY_RANDOM_BLINK, msgHandlerLedOverlayRandomBlink, nullptr},
                                    {W_MSG_TYPE_LED_LEVEL_SCALE, msgHandlerLedLevelScale, nullptr},
                                    {W_MSG_TYPE_NONE, nullptr, nullptr}},
                                   0, 0}};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE QUEUES
 * -------------------------------------------------------------- */

// Initialise messaging; returns a timer that is employed by
// messaging loops.
static int msgQueueInit()
{
    int fdOrErrorCode = 0;

    // Set up a tick to drive msgQueueLoop()
    fdOrErrorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fdOrErrorCode >= 0) {
        struct itimerspec timerSpec = {0};
        timerSpec.it_value.tv_nsec = W_MSG_QUEUE_TICK_TIMER_PERIOD_US * 1000;
        timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
        if (timerfd_settime(fdOrErrorCode, 0, &timerSpec, nullptr) != 0) {
            close(fdOrErrorCode);
            fdOrErrorCode = -errno;
            W_LOG_ERROR("unable to set messaging tick timer, error code %d.",
                        fdOrErrorCode);
        }
    } else {
        fdOrErrorCode = -errno;
        W_LOG_ERROR("unable to create messaging tick timer, error code %d.",
                    fdOrErrorCode);
    }

    return fdOrErrorCode;
}

// Push a message onto a queue.  body is copied so it can be passed
// in any which way (and it is up to the caller of msgQueueTryPop()
// to free the copied message body).  The addresses of any members
// of wMsgBody_t can be passed in, cast to wMsgBody_t *.
// I tried to do this properly as a union but gave up and cast
// instead.
static int msgQueuePush(wMsgQueueType_t queueType,
                        wMsgType_t msgType, wMsgBody_t *body)
{
    int errorCode = -EINVAL;

    if ((queueType < W_ARRAY_COUNT(gMsgQueue)) &&
        (msgType < W_ARRAY_COUNT(gMsgBodySize))) {
        errorCode = 0;
        wMsgQueue_t *msgQueue = &(gMsgQueue[queueType]);
        wMsgBody_t *bodyCopy = nullptr;
        if (gMsgBodySize[msgType] > 0) {
            errorCode = -ENOMEM;
            bodyCopy = (wMsgBody_t *) malloc(gMsgBodySize[msgType]);
            if (bodyCopy) {
                errorCode = 0;
                memcpy(bodyCopy, body, gMsgBodySize[msgType]);
            }
        }
        if (errorCode == 0) {
            wMsgContainer_t container = {.type = msgType,
                                         .body = bodyCopy};
            msgQueue->mutex->lock();
            errorCode = -ENOBUFS;
            unsigned int queueLength = msgQueue->containerList->size();
            if (queueLength < msgQueue->maxSize) {
                errorCode = queueLength + 1;
                try {
                    msgQueue->containerList->push_back(container);
                }
                catch(int x) {
                    errorCode = -x;
                }
            }
            msgQueue->mutex->unlock();
        }

        if (errorCode < 0) {
            if (bodyCopy) {
                free(bodyCopy);
            }
            W_LOG_ERROR("unable to push message type %d, body length %d,"
                        " to %s message queue (%d)!",
                        msgType, gMsgBodySize[msgType],
                        msgQueue->name, errorCode);
        }
    }

    return errorCode;
}

// Wait for a lock on a mutex for a given time; this should only be
// used by msgQueueClear(): msgQueueLoop() should run off the messaging
// timer to ensure that it sleeps properly.
//
// Note: see here:
// https://stackoverflow.com/questions/44190865/stdtimed-mutextry-lock-for-fails-immediately
// ...for why one can't use try_lock_for(), which would seem like
// the more obvious approach.
static bool msqQueueMutexTryLockFor(std::mutex *mutex, std::chrono::nanoseconds wait)
{
    bool gotLock = false;
    wTimeoutStart_t startTime = timeoutStart();

    while (mutex && !gotLock && !timeoutExpired(startTime, wait)) {
        gotLock = mutex->try_lock();
        if (!gotLock) {
           std::this_thread::sleep_for(std::chrono::microseconds(W_MSG_QUEUE_TICK_TIMER_PERIOD_US));
        }
    }

    return gotLock;
}

// Empty a message queue.
static int msgQueueClear(wMsgQueueType_t queueType, void *context)
{
    int errorCode = 0;
    wMsgContainer_t msg;

    if (queueType < W_ARRAY_COUNT(gMsgQueue)) {
        wMsgQueue_t *msgQueue = &(gMsgQueue[queueType]);
        if (msqQueueMutexTryLockFor(msgQueue->mutex,
                                    W_MSG_QUEUE_TRY_LOCK_WAIT)) {
            while (!msgQueue->containerList->empty()) {
                errorCode = 0;
                msg = msgQueue->containerList->front();
                msgQueue->containerList->pop_front();
                // See if there is a free() function
                wMsgHandler_t *handler = nullptr;
                for (unsigned int x = 0;
                     (x < W_ARRAY_COUNT(msgQueue->handler)) &&
                     (handler = &(msgQueue->handler[x])) &&
                     (handler->function != nullptr) &&
                     (handler->msgType != msg.type);
                     x++) {}
                if (handler && (handler->free)) {
                    // Call the free function
                    handler->free(msg.body, context);
                }
                // Now free the body
                free(msg.body);
            }
            msgQueue->mutex->unlock();
        } else {
            W_LOG_WARN("unable to lock %s message queue to clear it.",
                       msgQueue->name);
        }
    }

    return errorCode;
}

// Try to pop a message off a queue.  If a message is returned
// the caller MUST call free() on msg->body.
static int msgQueueTryPop(wMsgQueue_t *msgQueue,
                          wMsgContainer_t *msg)
{
    int errorCode = -EINVAL;

    if (msgQueue && msg) {
        errorCode = -EAGAIN;
        if (msgQueue->mutex->try_lock() &&
            !msgQueue->containerList->empty()) {
            *msg = msgQueue->containerList->front();
            msgQueue->containerList->pop_front();
            errorCode = 0;
        }
        msgQueue->mutex->unlock();
    }

    return errorCode;
}

// The message handler loop.
//
// Note: I tried a few ways of doing this:
// - the most obvious is to try_lock_for() on the mutex protecting the
//   message queue; however, try_lock_for() keeps on exiting spontanously,
//   wasting CPU cycles enormously.
// - next would be to do a try_lock() and then loop with a sleep_for() as
//   a kind of poll-interval if it fails, but that's clunky and sleep_for()
//   also has a habit of returning spontaneously.
// - experience with the GPIO stuff shows that running a timer that allows
//   us to block on a file descriptor really does sleep, so FD and poll
//   interval it is.
static void msgQueueLoop(wMsgQueueType_t queueType, int fd, void *context)
{
    if (queueType < W_ARRAY_COUNT(gMsgQueue)) {
        uint64_t numExpiries;
        struct pollfd pollFd[1] = {0};
        struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
        sigset_t sigMask;
        wMsgQueue_t *msgQueue = &(gMsgQueue[queueType]);
        wMsgContainer_t msg;
        pollFd[0].fd = fd;
        pollFd[0].events = POLLIN | POLLERR | POLLHUP;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);

        W_LOG_DEBUG("%s: message loop has started", msgQueue->name);

        while (!gTerminated) {
            // Block waiting for the messaging timer to go off for up to
            // a time, or for CTRL-C to land
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(fd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {
                // Pop all the messages waiting for us
                while (msgQueueTryPop(msgQueue, &msg) == 0) {
                    // Find the message handler
                    wMsgHandler_t *handler = nullptr;
                    for (unsigned int x = 0;
                         (x < W_ARRAY_COUNT(msgQueue->handler)) &&
                         (handler = &(msgQueue->handler[x])) &&
                         (handler->function != nullptr) &&
                         (handler->msgType != msg.type);
                         x++) {}
                    if (handler && (handler->function)) {
                        // Call the handler
                        handler->function(msg.body, context);
                        msgQueue->count++;
                    } else {
                        W_LOG_ERROR("%s: unhandled message type (%d)",
                                    msgQueue->name, msg.type);
                    }
                    // Free the message body now that we're done
                    free(msg.body);
                }
            }
        }
        W_LOG_DEBUG("%s: message loop has ended", msgQueue->name);
    } else {
        W_LOG_ERROR("message queue type out of range (%d, limit %d)",
                    queueType, W_ARRAY_COUNT(gMsgQueue));
    }
}

// Start a message queue thread.
static int msgQueueThreadStart(wMsgQueueType_t queueType,
                               int fd, void *context,
                               std::thread *thread)
{
    int errorCode = -EINVAL;

    if (thread && (queueType < W_ARRAY_COUNT(gMsgQueue))) {
        wMsgQueue_t *msgQueue = &(gMsgQueue[queueType]);
        // This will go bang if the thread could not be created
        *thread = std::thread(msgQueueLoop, queueType, fd, context);
        // Best effort, add the name so that it is displayed when debugging
        pthread_setname_np(thread->native_handle(), msgQueue->name);
        errorCode = 0;
    }

    return errorCode;
}

// Stop a message queue thread.
static void msgQueueThreadStop(wMsgQueueType_t queueType,
                               void *context,
                               std::thread *thread)
{
    if (thread && thread->joinable()) {
        thread->join();
    }
    msgQueueClear(queueType, context);
}

// Get the previousSize record for the given message queue, used
// by the caller for debugging queue build-ups.
static unsigned int msgQueuePreviousSizeGet(wMsgQueueType_t queueType)
{
    unsigned int previousSize = 0;

    if (queueType < W_ARRAY_COUNT(gMsgQueue)) {
        wMsgQueue_t *msgQueue = &(gMsgQueue[queueType]);
        previousSize = msgQueue->previousSize;
    }

    return previousSize;
}

// Set the previousSize record for the given message queue, used
// by the caller for debugging queue build-ups.
static void msgQueuePreviousSizeSet(wMsgQueueType_t queueType,
                                    unsigned int previousSize)
{
    if (queueType < W_ARRAY_COUNT(gMsgQueue)) {
        wMsgQueue_t *msgQueue = &(gMsgQueue[queueType]);
        msgQueue->previousSize = previousSize;
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
                    wMsgBodyImageBuffer_t buffer = {.width = width,
                                                    .height = height,
                                                    .stride = stride,
                                                    .sequence = metadata.sequence,
                                                    .data = data,
                                                    .length = dmaBufferLength};
                    unsigned int queueLength = msgQueuePush(W_MSG_QUEUE_IMAGE_PROCESSING,
                                                            W_MSG_TYPE_IMAGE_BUFFER,
                                                            (wMsgBody_t *) &buffer);
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

// Enable or disable motor control; a disabled motor will also be
// marked as uncalibrated since it may move freely when disabled.
static int motorEnable(wMotor_t *motor, bool enableNotDisable = true)
{
    int errorCode = gpioSet(motor->pinDisable, !enableNotDisable);

    if ((errorCode == 0) && !enableNotDisable) {
        // If disabling the motor, it is no longer calibrated
        motor->calibrated = false;
    }

    return errorCode;
}

// Enable or disable all motors; a disabled motor will also be
// marked as uncalibrated since it may move freely once disabled.
static int motorsEnable(bool enableNotDisable = true)
{
    int errorCode = 0;

    for (unsigned int x = 0; x < W_ARRAY_COUNT(gMotor); x++) {
        wMotor_t *motor = &(gMotor[x]);
        int y = motorEnable(motor, enableNotDisable);
        if (y < 0) {
            errorCode = y;
            W_LOG_ERROR("%s: error %sing motor.", motor->name,
                        enableNotDisable ? "enabl" : "disabl");
        }
    }

    return errorCode;
}

// Perform a step; will not move if at a limit; being at
// a limit does not constitute an error: supply stepTaken
// if you want to know the outcome.
static int motorStep(wMotor_t *motor, int step = 1, int *stepTaken = nullptr)
{
    int errorCode = -EINVAL;

    if (stepTaken) {
        *stepTaken = 0;
    }

    if (motor) {
        // Check for limits
        errorCode = 0;
        if (step > 0) {
            errorCode = gpioGet(motor->pinMax);
        } else if (step < 0) {
            errorCode = gpioGet(motor->pinMin);
        }

        if (errorCode == 1) {
            // A limit level of 1 means the pin remains in its default
            // pulled-up state, we can move

            // Set the correct direction
            unsigned int levelDirection = 0;
            if (step >= 0) {
                levelDirection = step;
            }
            if (motor->senseDirection < 0) {
                levelDirection = !levelDirection;
            }
            errorCode = gpioSet(motor->pinDirection, levelDirection);
            if (errorCode == 0) {
                // Wait a moment for the direction pin to settle
                std::this_thread::sleep_for(std::chrono::milliseconds(W_MOTOR_DIRECTION_WAIT_MS));
                // Send out a zero to one transition and wait
                errorCode = gpioSet(motor->pinStep, 0);
                if (errorCode == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(W_MOTOR_STEP_WAIT_MS));
                    errorCode = gpioSet(motor->pinStep, 1);
                    if (errorCode == 0) {
                        // Make sure we sit at a one for long enough
                        std::this_thread::sleep_for(std::chrono::milliseconds(W_MOTOR_STEP_WAIT_MS));
                    }
                }
            }
            if ((errorCode == 0) && (stepTaken)) {
                // We have taken a step
                *stepTaken = step;
            }
        } else if ((errorCode == 0) && (step != 0)) {
            W_LOG_DEBUG("%s: hit %s limit.", motor->name, step > 0 ? "max" : "min");
        }

        if (errorCode < 0) {
            W_LOG_ERROR("%s: error %d on step.", motor->name, errorCode);
        }
    }

    return errorCode;
}

// Try to move the given number of steps, returning
// the number actually stepped in stepsTaken; being short
// on steps does not constitute an error.  Will only move
// if calibrated unless evenIfUnCalibrated is true.
static int motorMove(wMotor_t *motor, int steps,
                     int *stepsTaken = nullptr,
                     bool evenIfUnCalibrated = false)
{
    int errorCode = -EINVAL;
    int step = 1;
    int stepsCompleted = 0;

    if (motor && (motor->calibrated || evenIfUnCalibrated)) {
        errorCode = 0;
        if (steps > 0) {
            if (motor->calibrated) {
                // Limit the steps against the calibrated maximum
                if (motor->now + steps > motor->max) {
                    steps = motor->max - motor->now;
                }
            } else {
                // Limit the steps against the hard-coded safety
                if (steps > (int) motor->safetyLimit) {
                    steps = motor->safetyLimit;
                }
            }
        } else if (steps < 0) {
            if (motor->calibrated) {
                // Limit the steps against the calibrated minimum
                if (motor->now + steps < motor->min) {
                    steps = motor->min - motor->now;
                }
            } else {
                // Limit the steps against the hard-coded safety
                if (steps < -((int) motor->safetyLimit)) {
                    steps = -motor->safetyLimit;
                }
            }
            step = -1;
        }

        if (motor->calibrated) {
            W_LOG_DEBUG("%s: moving %+d step(s).", motor->name, steps);
        } else {
            W_LOG_WARN("%s: uncalibrated movement of %+d step(s).",
                       motor->name, steps);
        }

        // Actually move
        int stepTaken = 1;
        for (int x = 0; (x < steps * step) && (stepTaken != 0) &&
                        (errorCode == 0); x++) {
            stepTaken = 0;
            errorCode = motorStep(motor, step, &stepTaken);
            if (errorCode == 0) {
                stepsCompleted += stepTaken;
            }
        }

        if (motor->calibrated) {
            motor->now += stepsCompleted;
            W_LOG_INFO("%d: now at position %d.", motor->name, motor->now);
        }

        if (stepsCompleted < steps) {
            W_LOG_WARN_START("%s: only %+d step(s) taken (%d short)",
                             motor->name, stepsCompleted, steps - stepsCompleted);
            if (motor->calibrated) {
                W_LOG_WARN_MORE(" motor now needs calibration");
            }
            W_LOG_WARN_MORE(".");
            W_LOG_WARN_END;
            motor->calibrated = false;
        }

        if (stepsTaken) {
            *stepsTaken = stepsCompleted;
        }
    }

    return errorCode;
}

// Send a motor to its rest position; will only do so if
// the motor is calibrated.  Not being able to get to the
// rest position _does_ constitute an error.
static int motorMoveToRest(wMotor_t *motor, int *stepsTaken = nullptr)
{
    int errorCode = -EINVAL;
    int steps = 0;
    int stepsCompleted = 0;

    if (motor && motor->calibrated) {
        errorCode = 0;
        switch (motor->restPosition) {
            case W_MOTOR_REST_POSITION_CENTRE:
                steps = -motor->now;
                break;
            case W_MOTOR_REST_POSITION_MAX:
                steps = motor->max - motor->now;
                break;
            case W_MOTOR_REST_POSITION_MIN:
                steps = motor->min - motor->now;
                break;
            default:
                break;
        }

        if (steps != 0) {
            errorCode = motorMove(motor, steps, &stepsCompleted, true);
            if (errorCode == 0) {
                if (stepsCompleted != steps) {
                    errorCode = -ENXIO;
                    W_LOG_ERROR("%s: unable to take %+d step(s) to %s"
                                 " rest position (only %+d step(s) taken)!",
                                 motor->name, steps,
                                 gMotorRestPositionStr[motor->restPosition],
                                 stepsCompleted);
                }
            } else {
                W_LOG_ERROR("%s: unable to get to rest position (error %d)!",
                            motor->name, errorCode);
            }
        }
        if (stepsTaken) {
            *stepsTaken = stepsCompleted;
        }
    }

    return errorCode;
}

// Calibrate the movement range of a motor.
static int motorCalibrate(wMotor_t *motor)
{
    int errorCode = 0;
    int steps = 0;

    motor->calibrated = false;
    // Move the full safety distance backwards to the min limit switch
    errorCode = motorMove(motor, -motor->safetyLimit, &steps, true);
    if (errorCode == 0) {
        if (steps > (int) -motor->safetyLimit) {
            steps = 0;
            // Do the same in the forward direction
            errorCode = motorMove(motor, motor->safetyLimit, &steps, true);
            if (errorCode == 0) {
                if (steps < ((int) motor->safetyLimit)) {
                    // steps is now the distance between the minimum
                    // and maximum limits: set the current position
                    // and the limits; the margin will be just inside
                    // the limit switches so that we can move without
                    // stressing them and we know that our movement
                    // has become innaccurate if we hit them
                    steps >>= 1;
                    motor->now = steps;
                    steps -= W_MOTOR_LIMIT_MARGIN_STEPS;
                    motor->max = steps;
                    motor->min = -steps;
                    motor->calibrated = true;
                    W_LOG_INFO("%s: calibrated range +/- %d step(s).",
                               motor->name, steps);
                } else {
                    W_LOG_ERROR("%s: unable to calibrate, moving %+d step(s)"
                                " from the max limit did not reach the min"
                                " limit switch.", motor->name,
                                motor->safetyLimit);
                }
            }
        } else {
            W_LOG_ERROR("%s: unable to calibrate, moving %+d step(s) did"
                        " not reach the max limit switch.", motor->name,
                        motor->safetyLimit);
        }
    }

    if ((errorCode == 0) && !motor->calibrated) {
        errorCode = -ENXIO;
    }

    return errorCode;
}

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
        W_LOG_INFO("calibrating limits of movement, STAND CLEAR!");
        // Now calibrate movement
        errorCode = motorsEnable();
        for (unsigned int x = 0; (x < W_ARRAY_COUNT(gMotor)) &&
                                 (errorCode == 0); x++) {
            errorCode = motorCalibrate(&(gMotor[x]));
        }
        if (errorCode == 0) {
            W_LOG_INFO("calibration successful, moving to rest position.");
            for (unsigned int x = 0; (x < W_ARRAY_COUNT(gMotor)) &&
                                     (errorCode == 0); x++) {
                errorCode = motorMoveToRest(&(gMotor[x]));
            }
        }
        if (errorCode != 0) {
            // Disable motors again if calibration or moving
            // to rest position failed
            motorsEnable(false);
        }
    }

    return errorCode;
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
 * STATIC FUNCTIONS: TESTS
 * -------------------------------------------------------------- */

// Run through a test sequence for the LEDs: everything must already
// have been initialised before this can be called.
static int testLeds()
{
    int errorCode = 0;
    const char *prefix = "LED TEST: ";

    W_LOG_INFO("%sSTART (will take a little while).", prefix);
    wMsgBodyLedModeConstant_t constant = {};

    W_LOG_INFO("%stesting constant mode.", prefix);
    W_LOG_INFO("%sboth LEDs ramped up over one second, left ahead of right.",
               prefix);
    constant.apply.led = W_LED_BOTH;
    constant.apply.offsetLeftToRightMs = 1000;
    constant.levelPercent = 10;
    constant.rampMs = 1000;
    for (constant.levelPercent = 10;
         (constant.levelPercent < 100) && !gTerminated;
         constant.levelPercent += 10) {
        msgQueuePush(W_MSG_QUEUE_LED,
                     W_MSG_TYPE_LED_MODE_CONSTANT, (wMsgBody_t *) &constant);
        sleep(2);
    }

    constant.apply.led = W_LED_LEFT;
    W_LOG_INFO("%s%s LED ramped down.", prefix, gLedStr[constant.apply.led]);
    for (constant.levelPercent = 100;
         (constant.levelPercent > 0) && !gTerminated;
         constant.levelPercent -= 10) {
        msgQueuePush(W_MSG_QUEUE_LED,
                     W_MSG_TYPE_LED_MODE_CONSTANT, (wMsgBody_t *) &constant);
        sleep(1);
    }
    constant.levelPercent = 0;
    msgQueuePush(W_MSG_QUEUE_LED,
                 W_MSG_TYPE_LED_MODE_CONSTANT, (wMsgBody_t *) &constant);
    sleep(1);

    constant.apply.led = W_LED_RIGHT;
    W_LOG_INFO("%s%s LED ramped down.", prefix, gLedStr[constant.apply.led]);
    for (constant.levelPercent = 100;
         (constant.levelPercent > 0) && !gTerminated;
         constant.levelPercent -= 10) {
        msgQueuePush(W_MSG_QUEUE_LED,
                     W_MSG_TYPE_LED_MODE_CONSTANT, (wMsgBody_t *) &constant);
        sleep(1);
    }
    constant.levelPercent = 0;
    msgQueuePush(W_MSG_QUEUE_LED,
                 W_MSG_TYPE_LED_MODE_CONSTANT, (wMsgBody_t *) &constant);
    sleep(1);

    W_LOG_INFO("%scompleted.", prefix);

    return errorCode;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// The entry point.
int main(int argc, char *argv[])
{
    int errorCode = -ENXIO;
    wCommandLineParameters_t commandLineParameters;
    wVideoEncodeContext_t videoEncode = {};
    AVStream *avStream = nullptr;
    int timerFd = -1;
    int pwmFd = -1;
    int msgFd = -1;
    std::thread gpioPwmThread;
    std::thread gpioReadThread;
    wLedContext_t ledContext = {};
    std::thread ledControlThread;
    std::thread controlThread;
    std::thread imageProcessingThread;
    std::thread videoEncodeThread;

    // Process the command-line parameters
    if (commandLineParse(argc, argv, &commandLineParameters) == 0) {
        commandLinePrintChoices(&commandLineParameters);
        // Capture CTRL-C so that we can exit in an organised fashion
        signal(SIGINT, terminateSignalHandler);

        // Initialise GPIOs, a thread driven by a timer to
        // monitor them, and a PWM timer to drive PWM
        // operation
        errorCode = gpioInit(&pwmFd);
        if (errorCode >= 0) {
            timerFd = errorCode;
            // Start the PWM thread
            gpioPwmThread = std::thread(gpioPwmLoop, pwmFd);
            pthread_setname_np(gpioPwmThread.native_handle(), "gpioPwmLoop");
            // Start the read thread
            gpioReadThread = std::thread(gpioReadLoop, timerFd);
            pthread_setname_np(gpioReadThread.native_handle(), "gpioReadLoop");
            // Set the thread priority for GPIO reads
            // to maximum as we never want to miss one
            struct sched_param scheduling;
            scheduling.sched_priority = sched_get_priority_max(SCHED_FIFO);
            errorCode = pthread_setschedparam(gpioReadThread.native_handle(),
                                              SCHED_FIFO, &scheduling);
            if (errorCode == 0) {
                // We should now be able to initialise the HW
                errorCode = hwInit();
            } else {
                W_LOG_ERROR("unable to set thread priority for GPIO reads"
                            " (error %d), maybe need sudo?", errorCode);
            }
        } else {
            W_LOG_ERROR("error %d initialising GPIOs!", errorCode);
        }

        if (errorCode == 0) {
            // Initialise messaging
            errorCode = msgQueueInit();
            if (errorCode >= 0) {
                msgFd = errorCode;
                errorCode = 0;
            }
        }

        if (errorCode == 0) {
            // Kick off the LED thread
            errorCode = ledInit(&ledContext);
        }

        if (errorCode == 0) {
            // Kick off an LED control thread
            msgQueueThreadStart(W_MSG_QUEUE_LED, msgFd, &ledContext, &ledControlThread);
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
                                                        std::string(W_DIR_SEPARATOR) +
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
                                             std::string(W_HLS_BASE_URL W_DIR_SEPARATOR +
                                                         commandLineParameters.outputDirectory +
                                                         W_DIR_SEPARATOR).c_str(), 0) == 0) &&
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
            gTerminated = true;
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
            W_LOG_ERROR("HW self test failure!");
        }

        // Disable the stepper motors
        motorsEnable(false);
        // Stop the GPIO tick timer
        // Make sure all threads know we have terminated
        gTerminated = true;
        if (timerFd >= 0) {
            if (gpioReadThread.joinable()) {
               gpioReadThread.join();
            }
            close(timerFd);
        }
        // Stop the GPIO PWM timer
        if (pwmFd >= 0) {
            if (gpioPwmThread.joinable()) {
               gpioPwmThread.join();
            }
            close(pwmFd);
        }
        // Give back the GPIOs
        gpioDeinit();

    } else {
        // Print help about the commad line, including the defaults
        commandLinePrintHelp(&commandLineParameters);
    }

    return errorCode;
}

// End of file
