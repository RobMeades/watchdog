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
 * @brief The watchdog application, main().
 *
 * Note: to run with maximum debug from libcamera, execute this program
 * as:
 *
 * LIBCAMERA_LOG_LEVELS=0 ./watchdog
 *
 * ...or to switch all debug output off:
 *
 * LIBCAMERA_LOG_LEVELS=3 ./watchdog
 *
 * The default is to run with  log level 1, which includes
 * information, warning and errors from libcamera, but not pure debug.
 */

#include <string>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <list>

#include <sys/mman.h>
#include <sys/time.h>

#include <libcamera/libcamera.h>

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

using namespace libcamera;
using namespace cv;

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
 * COMPILE-TIME MACROS: HLS VIDEO OUTPUT SETTINGS
 * -------------------------------------------------------------- */

#ifndef W_HLS_FILE_NAME_ROOT
// The root name for our HLS video files (.m3u8 and .ts).
# define W_HLS_FILE_NAME_ROOT "watchdog"
#endif

#ifndef W_HLS_PLAYLIST_FILE_NAME
// The playlist name to serve HLS video.
# define W_HLS_PLAYLIST_FILE_NAME W_HLS_FILE_NAME_ROOT ".m3u8"
#endif

#ifndef W_HLS_SEGMENT_DURATION_SECONDS
// The length of a segment in seconds.
# define W_HLS_SEGMENT_DURATION_SECONDS 5
#endif

#ifndef W_HLS_BASE_URL
// The URL to serve from.
# define W_HLS_BASE_URL "http://10.10.1.16/"
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: CAMERA-RELATED
 * -------------------------------------------------------------- */

#ifndef W_CAMERA_STREAM_ROLE
// The libcamera StreamRole to use as a basis for the video stream.
# define W_CAMERA_STREAM_ROLE StreamRole::VideoRecording
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

#ifndef W_CAMERA_FRAME_RATE_HERTZ
// Frames per second.
# define W_CAMERA_FRAME_RATE_HERTZ 25
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
 * COMPILE-TIME MACROS: LOGGING
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
 * TYPES
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
 * VARIABLES
 * -------------------------------------------------------------- */

// Pointer to camera: global as the requestCompleted() callback will use it.
static std::shared_ptr<Camera> gCamera = nullptr;

// Count of frames received from the camera.
static unsigned int gCameraStreamFrameCount = 0;

// Count of frames passed into video codec.
static unsigned int gVideoStreamFrameInputCount = 0;

// Count of frames received from the video codec.
static unsigned int gVideoStreamFrameOutputCount = 0;

// Remember the size of the frame list going to video, purely for
// debug purposes.
static unsigned int gVideoStreamFrameListSize = 0;

// Keep track of timing on the video stream.
static wMonitorTiming_t gCameraStreamMonitorTiming;

// Pointer to the OpenCV background subtractor: global as the
// requestCompleted() callback will use it.
static std::shared_ptr<BackgroundSubtractor> gBackgroundSubtractor = nullptr;

// A place to store the foreground mask for the OpenCV stream,
// globl as the requestCompleted() callback will populate it.
static Mat gForegroundMask;

// Linked list of video frames, FFmpeg-style.
static std::list<AVFrame *> gAvFrameList;

// Mutex to protect the linked list of FFmpeg-format video frames.
static std::mutex gAvFrameListMutex;

// Linked list of images, OpenCV-style.
static std::list<Mat *> gMatList;

// Mutex to protect the linked list of OpenCV-format images.
static std::mutex gMatListMutex;

// Flag to track that we're running (so that the video encode thread
// knows when to exit).
static bool gRunning = false;

// Array of log prefixes for the different log types.
static const char *gLogPrefix[] = {W_INFO, W_WARN, W_ERROR, W_DEBUG};

// Array of log destinations for the different log types.
static FILE *gLogDestination[] = {stdout, stdout, stderr, stdout};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LOGGING/MONITORING
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

// Configure a stream from the camear.
static bool cameraStreamConfigure(StreamConfiguration &streamCfg,
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

// Push a frame of video data onto the queue.
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
        // though there is actually no packing in our case; the width of the
        // plane is, for instance, 1920.  But in YUV420 only the Y plane is
        // at full resolution, the U and V planes are at half resolution
        // (e.g. 960), hence the divide by two for planes 1 and 2 below
        avFrame->linesize[0] = yStride;
        avFrame->linesize[1] = yStride >> 1;
        avFrame->linesize[2] = yStride >> 1;
        avFrame->time_base = W_VIDEO_STREAM_TIME_BASE_AVRATIONAL;
        avFrame->pts = sequenceNumber;
        avFrame->duration = 1;
        uint8_t *copy = (uint8_t *) malloc(length);
        if (copy) {
            memcpy(copy, data, length);
            avFrame->buf[0] = av_buffer_create(copy, length,
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
                unsigned int listSize = gAvFrameList.size();
                if (listSize < W_AVFRAME_LIST_MAX_ELEMENTS) {
                    errorCode = listSize + 1;
                    try {
                        gAvFrameList.push_back(avFrame);
                        // Keep track of timing for debug purposes
                        monitorTimingUpdate(&gCameraStreamMonitorTiming);
                        gVideoStreamFrameInputCount++;
                    }
                    catch(int x) {
                        errorCode = -x;
                    }
                }
                gAvFrameListMutex.unlock();
            }
        }

        if (errorCode < 0) {
            if (!avFrame->buf[0]) {
                // If we never employed the copy
                // need to free it explicitly.
                free(copy);
            }
            // This should cause avFrameFreeCallback() to be
            // called and free the copy that way
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
    gAvFrameListMutex.lock();
    gAvFrameList.clear();
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
            W_LOG_DEBUG("FFmpeg returned error %d.", errorCode);
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

    while (gRunning) {
        if (avFrameQueueTryPop(&avFrame) == 0) {
            // Procedure from https://ffmpeg.org/doxygen/7.0/group__lavc__encdec.html
           errorCode = avcodec_send_frame(codecContext, avFrame);
            if (errorCode == 0) {
                errorCode = videoOutput(codecContext, formatContext);
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
 * STATIC FUNCTIONS: CALLBACKS
 * -------------------------------------------------------------- */

// Handle a requestCompleted event from a camera.
static void requestCompleted(Request *request)
{
    if (request->status() != Request::RequestCancelled) {

       const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

       for (auto bufferPair : buffers) {
            FrameBuffer *buffer = bufferPair.second;
            const FrameMetadata &metadata = buffer->metadata();

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
#if 0
            // From the comment on this post:
            // https://stackoverflow.com/questions/44517828/transform-a-yuv420p-qvideoframe-into-grayscale-opencv-mat
            // ...we can bring in just the Y portion of the frame as effectively
            // a gray-scale image using CV_8UC1
            Mat frameOpenCv(height, width, CV_8UC1, dmaBuffer, stride);
            if (!frameOpenCv.empty()) {
                // Set JPEG image quality for OpenCV file write
                std::vector<int> imageCompression;
                imageCompression.push_back(IMWRITE_JPEG_QUALITY);
                imageCompression.push_back(40);

                // Update the background model of the motion detection stream
                //gBackgroundSubtractor->apply(frameOpenCv, gForegroundMask);
                //std::string sequenceStr = std::to_string(metadata.sequence);
                //std::string fileName = "lores" + sequenceStr + ".jpg";
                //imwrite(fileName, frameOpenCv, imageCompression);
            }
#endif
            // Stream the frame via FFmpeg
            unsigned int listSize = avFrameQueuePush(dmaBuffer, dmaBufferLength,
                                                     metadata.sequence,
                                                     width, height, stride);
            if ((gCameraStreamFrameCount % W_CAMERA_FRAME_RATE_HERTZ == 0) &&
                (listSize != gVideoStreamFrameListSize)) {
                // Print the size of the backlog once a second
                W_LOG_DEBUG("backlog %d frame(s)", listSize);
            }
            gVideoStreamFrameListSize = listSize;
            gCameraStreamFrameCount++;

            // Re-use the request
            request->reuse(Request::ReuseBuffers);
            gCamera->queueRequest(request);
        }
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// The entry point.
int main()
{
    int errorCode = -ENXIO;
    std::thread videoEncodeThread;
    AVFormatContext *avFormatContext = nullptr;
    AVCodecContext *avCodecContext = nullptr;
    AVStream *avStream = nullptr;

    // Create and start a camera manager instance
    std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
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
        std::unique_ptr<CameraConfiguration> cameraCfg = gCamera->generateConfiguration({W_CAMERA_STREAM_ROLE});
        cameraStreamConfigure(cameraCfg->at(0), W_CAMERA_STREAM_FORMAT,
                              W_CAMERA_STREAM_WIDTH_PIXELS,
                              W_CAMERA_STREAM_HEIGHT_PIXELS);

        // Validate and apply the configuration
        if (cameraCfg->validate() != CameraConfiguration::Valid) {
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
        FrameBufferAllocator *allocator = new FrameBufferAllocator(gCamera);
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
            std::vector<std::unique_ptr<Request>> requests;
            for (auto cfg: *cameraCfg) {
                Stream *stream = cfg.stream();
                const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
                for (unsigned int x = 0; (x < buffers.size()) && (errorCode == 0); x++) {
                    std::unique_ptr<Request> request = gCamera->createRequest();
                    if (request) {
                        const std::unique_ptr<FrameBuffer> &buffer = buffers[x];
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
                                               nullptr, W_HLS_PLAYLIST_FILE_NAME);
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
                    // options that _do_ apply.
                    if ((av_dict_set(&hlsOptions, "hls_time", W_STRINGIFY_QUOTED(W_HLS_SEGMENT_DURATION_SECONDS), 0) == 0) &&
                        (av_dict_set(&hlsOptions, "hls_base_url", W_HLS_BASE_URL, 0) == 0) &&
                        (av_dict_set(&hlsOptions, "hls_segment_type", "mpegts", 0) == 0) &&
                        (av_dict_set_int(&hlsOptions, "hls_allow_cache", 0, 0) == 0) &&
                        (av_dict_set(&hlsOptions, "hls_flags", "split_by_time+delete_segments+discont_start+omit_endlist", 0) == 0)) {
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
                                    // TODO whether this is correct or not: ensure a key frame every HLS segment
                                    // avCodecContext->keyint_min = (W_HLS_SEGMENT_DURATION_SECONDS >> 1) * W_CAMERA_FRAME_RATE_HERTZ;
                                    avCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
                                    avCodecContext->codec_id = AV_CODEC_ID_H264;
                                    avCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
                                    // This is needed to include the frame duration in the encoded
                                    // output, otherwise the HLS bit of av_interleaved_write_frame()
                                    // will emit a warning that frames having zero duration will mean
                                    // the HLS segment timing is orf
                                    avCodecContext->flags = AV_CODEC_FLAG_FRAME_DURATION;
                                    if ((avcodec_open2(avCodecContext, videoOutputCodec, nullptr) == 0) &&
                                        (avcodec_parameters_from_context(avStream->codecpar, avCodecContext) == 0) &&
                                        (avformat_write_header(avFormatContext, &hlsOptions) >= 0)) {
                                        // avformat_write_header() modifies hlsOptions to be
                                        // any options that weren't found
                                        const AVDictionaryEntry *entry = nullptr;
                                        while ((entry = av_dict_iterate(hlsOptions, entry))) {
                                            W_LOG_WARN("HLS key \"%s\" value \"%s\" not found.",
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
                    gBackgroundSubtractor = createBackgroundSubtractorMOG2();
                    if (gBackgroundSubtractor) {
                        // We have not yet set any of the controls for the camera;
                        // the only one we care about here is the frame rate,
                        // so that the settings above match.  There is a minimum
                        // and a maximum, setting both the same fixes the rate.
                        // We create a camera control list and pass it to
                        // the start() method when we start the camera.
                        ControlList cameraControls;
                        // Units are microseconds.
                        int64_t frameDurationLimit = 1000000 / W_CAMERA_FRAME_RATE_HERTZ;
                        cameraControls.set(controls::FrameDurationLimits, Span<const std::int64_t, 2>({frameDurationLimit,
                                                                                                       frameDurationLimit}));
                        // Attach the requestCompleted() handler
                        // function to its events and start the camera,
                        // everything else happens in the callback function
                        gCamera->requestCompleted.connect(requestCompleted);

                        // Kick off a thread to encode video frames
                        gRunning = true;
                        videoEncodeThread = std::thread{videoEncodeLoop,
                                                        avCodecContext,
                                                        avFormatContext}; 

                        // Remove any old files for a clean start
                        remove(W_HLS_PLAYLIST_FILE_NAME);
                        system("rm " W_HLS_FILE_NAME_ROOT "*.ts"); 

                        W_LOG_INFO("starting the camera and queueing requests (press <enter> to stop).");
                        gCamera->start(&cameraControls);
                        for (std::unique_ptr<Request> &request: requests) {
                            gCamera->queueRequest(request.get());
                        }

                        std::cin.get();

                        W_LOG_INFO("stopping the camera.");
                        gCamera->stop();
                    } else {
                        errorCode = -ENOMEM;
                        W_LOG_ERROR("unable to create background subtractor!");
                    }
                }
            }
        }

        // Tidy up
        W_LOG_DEBUG("tidying up.");
        // Stop the video encode thread
        gRunning = false;
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
        avFrameQueueClear();
        W_LOG_INFO("%d video frame(s) captured by camera, %d passed to encode (%d%%),"
                   " %d encoded video frame(s)).",
                   gCameraStreamFrameCount, gVideoStreamFrameInputCount, 
                   gVideoStreamFrameInputCount * 100 / gCameraStreamFrameCount,
                   gVideoStreamFrameOutputCount);
        W_LOG_INFO("average video frame gap (at video encoder input) over last %d frames"
                   " %lld ms (a rate of %lld frames/second), largest video"
                   " frame gap %lld ms.",
                   W_ARRAY_COUNT(gCameraStreamMonitorTiming.gap),
                   std::chrono::duration_cast<std::chrono::milliseconds>(gCameraStreamMonitorTiming.average).count(),
                   1000 / std::chrono::duration_cast<std::chrono::milliseconds>(gCameraStreamMonitorTiming.average).count(),
                   std::chrono::duration_cast<std::chrono::milliseconds>(gCameraStreamMonitorTiming.largest).count());
    } else {
        W_LOG_ERROR("no cameras found!");
    }

    // Tidy up
    cm->stop();
    return (int) errorCode;
}

// End of file
