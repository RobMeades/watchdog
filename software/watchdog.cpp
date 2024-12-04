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

#ifndef W_FRAME_DATA_QUEUE_MAX_ELEMENTS
// The maximum number of elements in the video frame data queue.
# define W_FRAME_DATA_QUEUE_MAX_ELEMENTS 100
#endif

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

#ifndef W_HLS_PLAYLIST_FILE_NAME
// The playlist name to service HLS video output.
# define W_HLS_PLAYLIST_FILE_NAME "watchdog.m3u8"
#endif

#ifndef W_HLS_SEGMENT_DURATION_SECONDS
// The length of a segment in seconds.
# define W_HLS_SEGMENT_DURATION_SECONDS 5
#endif

#ifndef W_HLS_BASE_URL
// The URL to serve from.
# define W_HLS_BASE_URL "http://10.10.1.16:1234/"
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: FORMATS
 * -------------------------------------------------------------- */

#ifndef W_STREAM_ROLE_VIDEO_RECORDING
// The libcamera StreamRole to use as a basis for the video
// recording stream.
# define W_STREAM_ROLE_VIDEO_RECORDING StreamRole::VideoRecording
#endif

#ifndef W_STREAM_FORMAT_VIDEO_RECORDING
// The pixel format for the video recording stream: must be
// YUV420 as that is what the video recoding code is expecting.
# define W_STREAM_FORMAT_VIDEO_RECORDING "YUV420"
#endif

#ifndef W_STREAM_FORMAT_HORIZONTAL_PIXELS_VIDEO_RECORDING
// Horizontal size of video recording stream in pixels.
# define W_STREAM_FORMAT_HORIZONTAL_PIXELS_VIDEO_RECORDING 1920
#endif

#ifndef W_STREAM_FORMAT_VERTICAL_PIXELS_VIDEO_RECORDING
// Vertical size of video recording stream in pixels.
# define W_STREAM_FORMAT_VERTICAL_PIXELS_VIDEO_RECORDING 1080
#endif

#ifndef W_STREAM_ROLE_MOTION_DETECTION
// The libcamera StreamRole to use as a basis for the motion
// detection stream.
# define W_STREAM_ROLE_MOTION_DETECTION StreamRole::Viewfinder
#endif

#ifndef W_STREAM_FORMAT_MOTION_DETECTION
// The pixel format for the motion detection stream:
// though RGB888 would be immediately importable by OpenCV,
// only a Raspbarry Pi 5 is able to provide the secondary
// stream as RGB, we have to use a Yxxx format but that is
// OK: as sandyol pointed out when I asked here:
//
// https://forums.raspberrypi.com/viewtopic.php?p=2273212#p2273212
//
// ...the Y stream is the luma information and that can
// be passed to OpenCV as a gray-scale image, which is
// all we need for motion detection.
# define W_STREAM_FORMAT_MOTION_DETECTION "YUV420"
#endif

#ifndef W_STREAM_FORMAT_HORIZONTAL_PIXELS_MOTION_DETECTION
// Horizontal size of the stream for motion detection in pixels.
# define W_STREAM_FORMAT_HORIZONTAL_PIXELS_MOTION_DETECTION 854
#endif

#ifndef W_STREAM_FORMAT_VERTICAL_PIXELS_MOTION_DETECTION
// Vertical size of the stream for motion detection in pixels.
# define W_STREAM_FORMAT_VERTICAL_PIXELS_MOTION_DETECTION 480
#endif

#ifndef W_FRAME_RATE_HERTZ
// Frames per second.
# define W_FRAME_RATE_HERTZ 25
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: LOGGING
 * -------------------------------------------------------------- */

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

// The type of video stream.  Values are important here since this
// is used as an index.
typedef enum {
    W_STREAM_TYPE_VIDEO_RECORDING = 0,
    W_STREAM_TYPE_MOTION_DETECTION = 1
} wStreamType_t;

// The types of log print.  Values are important as they are
// used as indexes into arrays.
typedef enum {
    W_LOG_TYPE_INFO = 0,
    W_LOG_TYPE_WARN = 1,
    W_LOG_TYPE_ERROR = 2,
    W_LOG_TYPE_DEBUG = 3
} wLogType_t;

// A buffer.
typedef struct {
    uint8_t *data;
    unsigned int length;
} wBuffer_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Pointer to camera: global as the requestCompleted() callback will use it.
static std::shared_ptr<Camera> gCamera = nullptr;

// The video output format context, global as the requestCompleted()
// callback will use it.
static  AVFormatContext *gVideoOutputContextFormat = nullptr;

// The video output codec context, global as the requestCompleted()
// callback will use it.
static AVCodecContext *gVideoOutputContextCodec = nullptr;

// The video output stream, global 'cos it is caught up the gubbins
// of stuff that is referenced by stuff called by requestCompleted().
static AVStream *gVideoOutputStream = nullptr;

// An AV frame for the video output, global as the requestCompleted()
// callback will use it.
static AVFrame *gVideoOutputFrame = nullptr;

// Count of video recording frames received.
static unsigned int gVideoRecordingFrameCount = 0;

// Count of video recording planes "held" by avcodec_send_frame().
static unsigned int gVideoRecordingFrameHeldCount = 0;

// Pointer to the OpenCV background subtractor: global as the
// requestCompleted() callback will use it.
static std::shared_ptr<BackgroundSubtractor> gBackgroundSubtractor = nullptr;

// A place to store the foreground mask for the OpenCV stream,
// globl as the requestCompleted() callback will populate it.
static Mat gForegroundMask;

// Linked list of frame data buffers.
static std::list<wBuffer_t> gFrameDataList;

// Mutex to protect the linked list of frame data buffers.
static std::mutex gFrameDataMutex;

// Flag to track that we're running (so that the video encode thread
// knows when to exit).
static bool gRunning;

// Names for the stream types, for debug purposes.
static const char *gStreamName[] = {"video recording", "motion detection"};

// Array of log prefixes for the different log types.
static const char *gLogPrefix[] = {W_INFO, W_WARN, W_ERROR, W_DEBUG};

// Arra of log destinations for the different log types.
static FILE *gLogDestination[] = {stdout, stdout, stderr, stdout};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LOGGING
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

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: FRAME DATA QUEUE MANAGEMENT
 * -------------------------------------------------------------- */

// Push a frame of data onto the queue.
static int frameDataQueuePush(uint8_t *data, unsigned int length)
{
    int errorCode = -ENOMEM;
    wBuffer_t buffer = {.data = data, .length = length};

    gFrameDataMutex.lock();
    if (gFrameDataList.size() < W_FRAME_DATA_QUEUE_MAX_ELEMENTS) {
        errorCode = 0;
        try {
            gFrameDataList.push_back(buffer);
        }
        catch(int x) {
            errorCode = -x;
        }
    }
    gFrameDataMutex.unlock();

    return errorCode;
}

// Try to pop a frame of data off the queue.
static int frameDataQueueTryPop(wBuffer_t *buffer)
{
    int errorCode = -EAGAIN;

    if (buffer && gFrameDataMutex.try_lock()) {
        if (!gFrameDataList.empty()) {
            *buffer = gFrameDataList.front();
            gFrameDataList.pop_front();
            errorCode = 0;
        }
        gFrameDataMutex.unlock();
    }

    return errorCode;
}

// Empty the data queue.
static void frameDataQueueClear()
{
    gFrameDataMutex.lock();
    gFrameDataList.clear();
    gFrameDataMutex.unlock();
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LIBCAMERA RELATED
 * -------------------------------------------------------------- */

// The conversion of a libcamera FrameBuffer to an OpenCV Mat requires
// the width, height and stride of the stream  So as to avoid having
// to search for this, we encode it into the cookie that is associated
// with a FrameBuffer when it is created, then the requestCompleted()
// callback can grab it.  See cookieDecode() for the reverse.
// We also encode which stream this is so that we can process
// the motion detection stream in a different way to the video
// recording  stream.
static uint64_t cookieEncode(unsigned int width, unsigned int height,
                             unsigned int stride,
                             wStreamType_t streamType)
{
    return ((uint64_t) width << 48) | ((uint64_t) (height & UINT16_MAX) << 32) |
            ((stride & UINT16_MAX) << 16) | (streamType & UINT16_MAX);
}

// Decode width, height, stride and stream type from a cookie; any
// pointer parameters may be NULL.
static void cookieDecode(uint64_t cookie, unsigned int *width,
                         unsigned int *height, unsigned int *stride,
                         wStreamType_t *streamType)
{
    if (width != nullptr) {
        *width = (cookie >> 48) & UINT16_MAX;
    }
    if (height != nullptr) {
        *height = (cookie >> 32) & UINT16_MAX;
    }
    if (stride != nullptr) {
        *stride = (cookie >> 16) & UINT16_MAX;
    }
    if (streamType != nullptr) {
        *streamType = (wStreamType_t) (cookie & UINT16_MAX);
    }
}

// Configure a stream.
static bool streamConfigure(wStreamType_t streamType,
                            StreamConfiguration &streamCfg,
                            std::string pixelFormatStr,
                            unsigned int widthPixels,
                            unsigned int heightPixels)
{
    bool formatFound = false;
    bool sizeFound = false;

    W_LOG_DEBUG("desired %s stream configuration %dx%d-%s.",
                gStreamName[streamType], widthPixels,
                heightPixels, pixelFormatStr.c_str());

    // Print out the current configuration
    W_LOG_DEBUG("existing %s stream configuration %s.",
                gStreamName[streamType],
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
        W_LOG_DEBUG("nearest %s stream configuration %s.",
                    gStreamName[streamType],
                    streamCfg.toString().c_str());
    }

    return formatFound && sizeFound;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: FFMPEG RELATED
 * -------------------------------------------------------------- */

// Callback for FFmpeg to call when it has finished with a buffer;
// the opaque pointer should be the value of gVideoRecordingFrameCount
// when av_buffer_create() was called, for debug purposes.
static void avFrameBufferFreeCallback(void *opaque, uint8_t *data)
{
    free(data);
    gVideoRecordingFrameHeldCount--;
    W_LOG_DEBUG("Video codec is done with frame %u, %u frame(s) still held.",
                (unsigned int) opaque, gVideoRecordingFrameHeldCount);
}

// Flush the video output.
static int videoOutputFlush()
{
    int errorCode = 0;

    W_LOG_DEBUG("flushing codec.");

    if ((gVideoOutputContextCodec != nullptr) &&
        (gVideoOutputContextFormat != nullptr)) {
        errorCode = -ENOMEM;
        AVPacket *packet = av_packet_alloc();
        if (packet) {
            // This puts the codec into flush mode
            errorCode = avcodec_send_frame(gVideoOutputContextCodec, nullptr);
            // Read out packets into we get back EOF
            while ((errorCode == 0) &&
                   (avcodec_receive_packet(gVideoOutputContextCodec, packet) != AVERROR_EOF)) {
                errorCode = av_interleaved_write_frame(gVideoOutputContextFormat, packet);
                av_packet_unref(packet);
            }

            av_packet_free(&packet);
        }

        // In case we want to use the codec again
        avcodec_flush_buffers(gVideoOutputContextCodec);
    }

    return errorCode;
}


// Video encode task/thread/thing.
static void videoEncode()
{
    wBuffer_t buffer;
    int32_t errorCode;

    while (gRunning) {
        if (frameDataQueueTryPop(&buffer) == 0) {
            errorCode = -ENOMEM;
            AVPacket *packet = av_packet_alloc();
            if (packet) {
                // The arrays here are quite confusing.  The entire
                // YUV420 data, packed in its own sweet way, is in
                // data[0].  buf[0] is a reference to data[0] that
                // carries referencing data; there is one reference
                // when av_buffer_create() returns, more may be added,
                // later.  When there are no references left the buffer
                // is free and avFrameBufferFreeCallback() should be
                // called by FFmpeg.  The locations of the Y, U and V
                // portions in the data are set by the linesize array,
                // which was set up originally when we created the
                // frame buffer.
                gVideoOutputFrame->buf[0] = av_buffer_create(buffer.data, buffer.length,
                                                             avFrameBufferFreeCallback,
                                                             (void *) gVideoRecordingFrameCount,
                                                             0);
                if (gVideoOutputFrame->buf[0]) {
                    errorCode = 0;
                }
                if (errorCode == 0) {
                    errorCode =  av_image_fill_pointers(gVideoOutputFrame->data, AV_PIX_FMT_YUV420P,
                                                        gVideoOutputFrame->height,
                                                        gVideoOutputFrame->buf[0]->data,
                                                        gVideoOutputFrame->linesize);
                }
                if (errorCode >= 0) {
                    errorCode = av_frame_make_writable(gVideoOutputFrame);
                }
                // Procedure from https://ffmpeg.org/doxygen/7.0/group__lavc__encdec.html
                if (errorCode == 0) {
                    gVideoOutputFrame->pts = gVideoRecordingFrameCount * (gVideoOutputContextFormat->streams[0]->time_base.den) / W_FRAME_RATE_HERTZ;
                    W_LOG_DEBUG("### calling avcodec_send_frame() for video recording frame %d.", gVideoRecordingFrameCount);
                    errorCode = avcodec_send_frame(gVideoOutputContextCodec, gVideoOutputFrame);
                }
                if (errorCode == 0) {
                    gVideoRecordingFrameHeldCount++;
                    // Call avcodec_receive_packet until it returns AVERROR(EAGAIN),
                    // meaning it needs to be fed more input
                    W_LOG_DEBUG("### calling avcodec_receive_packet().");
                    while((errorCode == 0) && (errorCode = avcodec_receive_packet(gVideoOutputContextCodec, packet) == 0)) {
                        W_LOG_DEBUG("### calling av_interleaed_write_frame().");
                        errorCode = av_interleaved_write_frame(gVideoOutputContextFormat, packet);
                        W_LOG_DEBUG("### write frame returned %d.", errorCode);
                    }
                }
                if ((errorCode != 0) && (errorCode != AVERROR(EAGAIN))) {
                    W_LOG_ERROR("error %d from FFmpeg!", errorCode);
                }
                av_packet_free(&packet);
            } else {
                 W_LOG_ERROR("unable to allocate packet for FFmpeg encode!");
            }
        }
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

            W_LOG_DEBUG("requestCompleted() sequence number %06d started.",
                        metadata.sequence);
            // Grab the stream's width, height, stride and which stream
            // this is, all of which is encoded in the buffer's cookie
            // when we associated it with the stream
            unsigned int width;
            unsigned int height;
            unsigned int stride;
            wStreamType_t streamType;
            cookieDecode(buffer->cookie(), &width, &height, &stride, &streamType);

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
            // Now do the OpenCV or FFmpeg thing, depending on the stream type
            // the buffer came from
            switch (streamType) {
                case W_STREAM_TYPE_MOTION_DETECTION:
                {
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
                }
                break;
                case W_STREAM_TYPE_VIDEO_RECORDING:
                {
                    // Must be a video recording frame: stream it!

                    // It would be lovely not to copy here but the video codec
                    // requires many (e.g. 50) frames of input before it starts
                    // producing output so we have to take copies of the three
                    // planes of data (Y, U and V), which are at their various
                    // offsets in the DMA buffer.  These copies will be free'd
                    // by avBufferFreePlaneCallback(), which FFmpeg should call
                    // when it is done with the buffer.
                    uint8_t *data = (uint8_t *) malloc(dmaBufferLength);
                    if (data) {
                        memcpy(data, dmaBuffer, dmaBufferLength);
                        if (frameDataQueuePush(data, dmaBufferLength) != 0) {
                            free(data);
                            W_LOG_ERROR("unable to push to data queue!");
                        }
                    } else {
                        W_LOG_ERROR("unable to allocate %u bytes for video frame!", dmaBufferLength);
                    }
                    gVideoRecordingFrameCount++;
                }
                break;
                default:
                    W_LOG_ERROR("unknown stream type %d!", streamType);
                    break;
            }

            // Print out some metadata just so that we can see what's going on
            W_LOG_DEBUG_START("%s sequence number %06d, bytes used: ",
                              gStreamName[streamType], metadata.sequence);
            unsigned int x = 0;
            for (const FrameMetadata::Plane &plane: metadata.planes()) {
                if (x > 0) {
                    W_LOG_DEBUG_MORE("/");
                }
                W_LOG_DEBUG_MORE("%d", plane.bytesused);
                x++;
            }
            W_LOG_DEBUG_MORE(".");
            W_LOG_DEBUG_END;

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

        // Configure the camera with two streams: order is
        // important, so that we can pick up the given configuration
        // with a known index (index 0 is the first in the list)
        std::unique_ptr<CameraConfiguration> cameraCfg = gCamera->generateConfiguration({W_STREAM_ROLE_VIDEO_RECORDING,
                                                                                         W_STREAM_ROLE_MOTION_DETECTION});
        // First configure the video recording stream
        streamConfigure(W_STREAM_TYPE_VIDEO_RECORDING,
                        cameraCfg->at(W_STREAM_TYPE_VIDEO_RECORDING),
                        W_STREAM_FORMAT_VIDEO_RECORDING,
                        W_STREAM_FORMAT_HORIZONTAL_PIXELS_VIDEO_RECORDING,
                        W_STREAM_FORMAT_VERTICAL_PIXELS_VIDEO_RECORDING);
        // Then configure the motion detection stream
        streamConfigure(W_STREAM_TYPE_MOTION_DETECTION,
                        cameraCfg->at(W_STREAM_TYPE_MOTION_DETECTION),
                        W_STREAM_FORMAT_MOTION_DETECTION,
                        W_STREAM_FORMAT_HORIZONTAL_PIXELS_MOTION_DETECTION,
                        W_STREAM_FORMAT_VERTICAL_PIXELS_MOTION_DETECTION);

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
            W_LOG_DEBUG("creating requests to the camera for each stream using the allocated buffers.");
            // Create a queue of requests on each stream using the allocated buffers
            std::vector<std::unique_ptr<Request>> requests;
            int streamIndex = 0;
            for (auto cfg: *cameraCfg) {
                Stream *stream = cfg.stream();
                const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);

                for (unsigned int x = 0; (x < buffers.size()) && (errorCode == 0); x++) {
                    std::unique_ptr<Request> request = gCamera->createRequest();
                    if (request) {
                        const std::unique_ptr<FrameBuffer> &buffer = buffers[x];
                        errorCode = request->addBuffer(stream, buffer.get());
                        if (errorCode == 0) {
                            // Encode the width, height, stride and index of the
                            // stream into the cookie of the FrameBuffer as we will
                            // need that information later when converting the
                            // FrameBuffer to a form that OpenCV or FFmpeg understands
                            buffer->setCookie(cookieEncode(stream->configuration().size.width,
                                                           stream->configuration().size.height,
                                                           stream->configuration().stride,
                                                           (wStreamType_t) streamIndex));
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
                streamIndex++;
            }
            if (errorCode == 0) {
                errorCode = -ENOMEM;
                // That's got pretty much all of the libcamera stuff, the camera
                // setup, done.  Now set up the output stream for video recording
                // using FFmpeg, format being HLS containing H.264-encoded data.
                const AVOutputFormat *videoOutputFormat = av_guess_format("hls", nullptr, nullptr);
                avformat_alloc_output_context2(&gVideoOutputContextFormat, videoOutputFormat,
                                               nullptr, W_HLS_PLAYLIST_FILE_NAME);
                if (gVideoOutputContextFormat) {
                    // Configure the HLS options
                    AVDictionary *hlsOptions = nullptr;
                    if ((av_dict_set(&hlsOptions, "hls_time", W_STRINGIFY_QUOTED(W_HLS_SEGMENT_DURATION_SECONDS), 0) == 0) &&
                        (av_dict_set(&hlsOptions, "hls_base_url", W_HLS_BASE_URL, 0) == 0) &&
                        (av_dict_set(&hlsOptions, "segment_format", "mpegts", 0) == 0) && // MPEG-TS transport stream
                        (av_dict_set(&hlsOptions, "segment_list_type", "m3u8", 0) == 0) &&
                        (av_dict_set(&hlsOptions, "segment_list", W_HLS_PLAYLIST_FILE_NAME, 0) == 0) &&
                        (av_dict_set_int(&hlsOptions, "segment_list_size", 0, 0) == 0) &&
                        (av_dict_set(&hlsOptions, "segment_time_delta", "1.0", 0) == 0) &&
                        (av_dict_set(&hlsOptions, "segment_time", W_STRINGIFY_QUOTED(W_HLS_SEGMENT_DURATION_SECONDS) ".0", 0) == 0) &&
                        (av_dict_set(&hlsOptions, "segment_list_flags", "cache+live", 0) == 0)) {
                        //  Set up the H264 video output stream over HLS
                        gVideoOutputStream = avformat_new_stream(gVideoOutputContextFormat, nullptr);
                        if (gVideoOutputStream) {
                            errorCode = -ENODEV;
                            const AVCodec *videoOutputCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
                            if (videoOutputCodec) {
                                errorCode = -ENOMEM;
                                gVideoOutputContextCodec = avcodec_alloc_context3(videoOutputCodec);
                                if (gVideoOutputContextCodec) {
                                    gVideoOutputContextCodec->width = cameraCfg->at(W_STREAM_TYPE_VIDEO_RECORDING).size.width;
                                    gVideoOutputContextCodec->height = cameraCfg->at(W_STREAM_TYPE_VIDEO_RECORDING).size.height;
                                    gVideoOutputContextCodec->time_base = av_make_q(1, W_FRAME_RATE_HERTZ);
                                    gVideoOutputContextCodec->framerate = av_make_q(W_FRAME_RATE_HERTZ, 1);
                                    // TODO whether this is correct or not: ensure a key frame every HLS segment
                                    gVideoOutputContextCodec->keyint_min = W_HLS_SEGMENT_DURATION_SECONDS * W_FRAME_RATE_HERTZ;
                                    gVideoOutputContextCodec->pix_fmt = AV_PIX_FMT_YUV420P;
                                    gVideoOutputContextCodec->codec_id = AV_CODEC_ID_H264;
                                    gVideoOutputContextCodec->codec_type = AVMEDIA_TYPE_VIDEO;
                                    if ((avcodec_open2(gVideoOutputContextCodec, videoOutputCodec, nullptr) == 0) &&
                                        (avcodec_parameters_from_context(gVideoOutputStream->codecpar, gVideoOutputContextCodec) == 0) &&
                                        (avformat_write_header(gVideoOutputContextFormat, &hlsOptions) >= 0)) {
                                        // Set up an FFmpeg AV frame for the video output
                                        gVideoOutputFrame = av_frame_alloc();
                                        if (gVideoOutputFrame) {
                                            gVideoOutputFrame->format = AV_PIX_FMT_YUV420P;
                                            gVideoOutputFrame->width = gVideoOutputContextCodec->width;
                                            gVideoOutputFrame->height = gVideoOutputContextCodec->height;
                                            // Each line size is the width of a plane (Y, U or V) plus packing,
                                            // though there is actually no packing in our case; the width of the
                                            // plane is, for instance, 1920.  But in YUV420 only the Y plane is
                                            // at full resolution, the U and V planes are at half resolution
                                            // (e.g. 960), hence the divide by two for planes 1 and 2 below
                                            gVideoOutputFrame->linesize[0] = cameraCfg->at(W_STREAM_TYPE_VIDEO_RECORDING).stride;
                                            gVideoOutputFrame->linesize[1] = cameraCfg->at(W_STREAM_TYPE_VIDEO_RECORDING).stride >> 1;
                                            gVideoOutputFrame->linesize[2] = gVideoOutputFrame->linesize[1];
                                            // We now have all the FFmpeg bits sorted except any actual
                                            // buffers within the frame: those will be added when the
                                            // requestCompleted() callback is called with a frame from
                                            // the camera
                                            errorCode = 0;
                                        } else {
                                            W_LOG_ERROR("unable to allocate memory for video output frame!");
                                        }
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
                        unsigned int frameDurationLimit = W_FRAME_RATE_HERTZ * 1000;
                        cameraControls.set(controls::FrameDurationLimits, libcamera::Span<const std::int64_t, 2>({frameDurationLimit,
                                                                                                                  frameDurationLimit}));
                        // Attach the requestCompleted() handler
                        // function to its events and start the camera,
                        // everything else happens in the callback function
                        gCamera->requestCompleted.connect(requestCompleted);

                        // Kick off a thread to encode video frames
                        gRunning = true;
                        videoEncodeThread = std::thread{videoEncode}; 

                        W_LOG_INFO("starting the camera and queueing requests for 3 seconds.");
                        gCamera->start(&cameraControls);
                        for (std::unique_ptr<Request> &request: requests) {
                            gCamera->queueRequest(request.get());
                        }

                        std::this_thread::sleep_for(std::chrono::seconds(3));

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
        videoOutputFlush();
        // Stop the video encode thread
        gRunning = false;
        if (videoEncodeThread.joinable()) {
           videoEncodeThread.join();
        }
        if (gVideoOutputContextFormat) {
            av_write_trailer(gVideoOutputContextFormat);
        }
        av_frame_free(&gVideoOutputFrame);
        avcodec_free_context(&gVideoOutputContextCodec);
        if (gVideoOutputContextFormat) {
            avio_closep(&gVideoOutputContextFormat->pb);
            avformat_free_context(gVideoOutputContextFormat);
        }
        for (auto cfg: *cameraCfg) {
            allocator->free(cfg.stream());
        }
        delete allocator;
        gCamera->release();
        gCamera.reset();
        frameDataQueueClear();
        W_LOG_DEBUG("at and, %u frames not released by avcodec_send_frame()",
                    gVideoRecordingFrameHeldCount);
    } else {
        W_LOG_ERROR("no cameras found!");
    }

    // Tidy up
    cm->stop();
    return (int) errorCode;
}

// End of file
