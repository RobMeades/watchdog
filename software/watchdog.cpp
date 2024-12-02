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

#include <sys/mman.h>

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
using namespace std::chrono_literals; // TODO: delete this when we don't use the delay thingy any more
using namespace cv;


/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: MISC
 * -------------------------------------------------------------- */

// Compute the number of elements in an array.
#define W_ARRAY_COUNT(array) (sizeof(array) / sizeof(array[0]))

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
# define W_FRAME_RATE_HERTZ 30
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

// Prefixes for info, warning and error strings.
#define W_INFO W_ANSI_COLOUR_BRIGHT_GREEN "INFO " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG " " W_ANSI_COLOUR_RESET
#define W_WARN W_ANSI_COLOUR_BRIGHT_YELLOW "WARN " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG " " W_ANSI_COLOUR_RESET
#define W_ERROR W_ANSI_COLOUR_BRIGHT_RED "ERROR " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG " " W_ANSI_COLOUR_RESET

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

// The type of video stream.  Values are important here since this
// is used as an index.
typedef enum {
    W_STREAM_TYPE_VIDEO_RECORDING = 0,
    W_STREAM_TYPE_MOTION_DETECTION = 1
} wStreamType_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Pointer to camera: global as the requestCompleted() callback will use it.
static std::shared_ptr<Camera> gCamera = nullptr;

// The video output format context, global as the bufferCompleted()
// callback will use it.
static  AVFormatContext *gVideoOutputContextFormat = nullptr;

// The video output codec context, global as the bufferCompleted()
// callback will use it.
static AVCodecContext *gVideoOutputContextCodec = nullptr;

// The video output stream, global 'cos it is caught up the gubbins
// of stuff that is referenced by stuff called by bufferCompleted().
static AVStream *gVideoOutputStream = nullptr;

// An AV frame for the video output, global as the bufferCompleted()
// callback will use it.
static AVFrame *gVideoOutputFrame = nullptr;

// Pointer to the OpenCV background subtractor: global as the
// bufferCompleted() callback will use it.
static std::shared_ptr<BackgroundSubtractor> gBackgroundSubtractor = nullptr;

// A place to store the foreground mask for the OpenCV stream,
// globl as the bufferCompleted() callback will populate it.
static Mat gForegroundMask;

// Names for the stream types, for debug purposes.
static const char *gStreamName[] = {"video recording", "motion detection"};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MISC
 * -------------------------------------------------------------- */

// The conversion of a libcamera FrameBuffer to an OpenCV Mat requires
// the width, height and stride of the stream  So as to avoid having
// to search for this, we encode it into the cookie that is associated
// with a FrameBuffer when it is created, then the bufferCompleted()
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

    std::cout << W_INFO "desired " << gStreamName[streamType]
              << " stream configuration (" << streamType << "): "
              << widthPixels << "x" << heightPixels << "-"
              << pixelFormatStr << std::endl;

    // Print out the current configuration
    std::cout << W_INFO "existing " << gStreamName[streamType]
              << " stream configuration (" << streamType << "): "
              << streamCfg.toString() << std::endl;

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
        std::cerr << W_ERROR "format " << pixelFormatStr << " not found, possible format(s): ";
        unsigned int x = 0;
        for (auto pixelFormat: streamCfg.formats().pixelformats()) {
            if (x > 0) {
                std::cout << ", ";
            }
            std::cout << pixelFormat.toString();
            x++;
        }
        std::cout << "." << std::endl;
    } else {
        if (!sizeFound) {
            std::cerr << W_ERROR "size " << widthPixels << "x" << heightPixels
                      << " not found, possible size(s): ";
            unsigned int x = 0;
            for (auto size: streamCfg.formats().sizes(streamCfg.pixelFormat)) {
                if (x > 0) {
                    std::cout << ", ";
                }
                std::cout << size.toString();
                x++;
            }
            std::cout << "." << std::endl;
        }
    }

    if (formatFound && sizeFound) {
        // Print where we ended up
        std::cout << W_INFO "nearest " << gStreamName[streamType]
                  << " stream configuration (" << streamType
                  << "): " << streamCfg.toString() << std::endl;
    }

    return formatFound && sizeFound;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: CALLBACKS
 * -------------------------------------------------------------- */

// Handle a bufferCompleted event (which occurs before a
// requestCompleted event) from a camera.
static void bufferCompleted(Request *request, FrameBuffer *buffer)
{
    if (request->status() != Request::RequestCancelled) {
        const FrameMetadata &metadata = buffer->metadata();

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
                    std::string sequenceStr = std::to_string(metadata.sequence);
                    std::string fileName = "lores" + sequenceStr + ".jpg";
                    imwrite(fileName, frameOpenCv, imageCompression);
                }
            }
            break;
            case W_STREAM_TYPE_VIDEO_RECORDING:
            {
                // Must be a video recording frame: stream it!
                AVPacket *packet = av_packet_alloc();
                if (packet) {
                    // Point the FFmpeg frame buffer at the three data planes,
                    // Y, U and V, which are at their various offsets in the
                    // DMA buffer (no copying)
                    for (unsigned int plane = 0; plane < 3; plane++) {
                        gVideoOutputFrame->buf[plane] = av_buffer_create(dmaBuffer + buffer->planes()[plane].offset,
                                                                         buffer->planes()[plane].length,
                                                                         av_buffer_default_free,
                                                                         nullptr, 0);
                    }
                    av_image_fill_pointers(gVideoOutputFrame->data, AV_PIX_FMT_YUV420P,
                                           gVideoOutputFrame->height,
                                           gVideoOutputFrame->buf[0]->data,
                                           gVideoOutputFrame->linesize);
                    av_frame_make_writable(gVideoOutputFrame);
                    gVideoOutputFrame->pts = metadata.sequence * (gVideoOutputContextFormat->streams[0]->time_base.den) / W_FRAME_RATE_HERTZ;
                    if (avcodec_send_frame(gVideoOutputContextCodec, gVideoOutputFrame) == 0) {
                        if (avcodec_receive_packet(gVideoOutputContextCodec, packet) == 0) {
                            av_interleaved_write_frame(gVideoOutputContextFormat, packet);
                        }
                    }
                    av_packet_free(&packet);
                }
            }
            break;
            default:
                std::cerr << W_ERROR "unknown stream type (" << streamType << ")!" << std::endl;
                break;
        }

        // Print out some metadata just so that we can see what's going on
        std::cout << W_INFO "seq: " << std::setw(6) << std::setfill('0')
                  << metadata.sequence << " bytes used: ";
        unsigned int x = 0;
        for (const FrameMetadata::Plane &plane: metadata.planes()) {
            if (x > 0) {
                std::cout << "/";
            }
            std::cout << plane.bytesused;
            x++;
        }
        std::cout << std::endl;
    }
}

// Handle a requestCompleted event from a camera.
static void requestCompleted(Request *request)
{
    // Re-use the request
    request->reuse(Request::ReuseBuffers);
    gCamera->queueRequest(request);
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// The entry point.
int main()
{
    int errorCode = -ENXIO;

    // Create and start a camera manager instance
    std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
    cm->start();

    // List the available cameras
    for (auto const &camera: cm->cameras()) {
        std::cout << W_INFO "found camera ID " << camera->id()
                  << " with properties:" << std::endl;
        auto cameraProperties =  camera->properties();
        auto idMap = cameraProperties.idMap();
        for(auto &controlValue: cameraProperties) {
            auto controlId = idMap->at(controlValue.first);
            std::string value = controlValue.second.toString();
            std::cout << "  " << std::setw(6) << controlValue.first
                      << " [" << controlId->name() << "]" << ": " << value
                      << std::endl;
        }
    }

    // Acquire the first (and probably only) camera
    auto cameras = cm->cameras();
    if (!cameras.empty()) {
        std::string cameraId = cameras[0]->id();
        std::cout << W_INFO "acquiring camera " << cameraId << std::endl;
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
            std::cout << W_WARN "libcamera will adjust those values." << std::endl;
        }
        gCamera->configure(cameraCfg.get());

        std::cout << W_INFO "validated/applied camera configuration:" << std::endl;
        for (std::size_t x = 0; x < cameraCfg->size(); x++) {
            std::cout << "  " << x << ": " << cameraCfg->at(x).toString() << std::endl;
        }

        // Allocate frame buffers
        FrameBufferAllocator *allocator = new FrameBufferAllocator(gCamera);
        errorCode = 0;
        for (auto cfg = cameraCfg->begin(); (cfg != cameraCfg->end()) && (errorCode == 0); cfg++) {
            errorCode = allocator->allocate(cfg->stream());
            if (errorCode >= 0) {
                std::cout << W_INFO "allocated " << errorCode << " buffer(s) for stream "
                          << cfg->toString() << std::endl;
                errorCode = 0;
            } else {
                std::cerr << W_ERROR "unable to allocate frame buffers (error code "
                          << errorCode << ")!" << std::endl;
            }
        }
        if (errorCode == 0) {
            std::cout << W_INFO "creating requests to the camera for each stream using the allocated buffers."
                      << std::endl;
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
                            // FrameBuffer to a form that OpenCV understands
                            buffer->setCookie(cookieEncode(stream->configuration().size.width,
                                                           stream->configuration().size.height,
                                                           stream->configuration().stride,
                                                           (wStreamType_t) streamIndex));
                            requests.push_back(std::move(request));
                        } else {
                            std::cerr << W_ERROR "can't attach buffer to request (" << errorCode << ")!"
                                      << std::endl;
                        }
                    } else {
                        errorCode = -ENOMEM;
                        std::cerr << W_ERROR "unable to create request!" << std::endl;
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
                                            // (e.g. 960), hence the divide by two below
                                            gVideoOutputFrame->linesize[0] = cameraCfg->at(W_STREAM_TYPE_VIDEO_RECORDING).stride;
                                            gVideoOutputFrame->linesize[1] = cameraCfg->at(W_STREAM_TYPE_VIDEO_RECORDING).stride >> 1;
                                            gVideoOutputFrame->linesize[2] = gVideoOutputFrame->linesize[1];
                                            // We now have all the FFmpeg bits sorted except any actual
                                            // buffers within the frame: those will be added when the
                                            // bufferCompleted() callback is called with a frame from
                                            // the camera
                                            errorCode = 0;
                                        } else {
                                            std::cerr << W_ERROR "unable to allocate memory for video output frame!" << std::endl;
                                        }
                                    } else {
                                        std::cerr << W_ERROR "unable to either open video codec or write AV format header!" << std::endl;
                                    }
                                } else {
                                    std::cerr << W_ERROR "unable to allocate memory for video output context!" << std::endl;
                                }
                            } else {
                                std::cerr << W_ERROR "unable to find H.264 codec in FFmpeg!" << std::endl;
                            }
                        } else {
                            std::cerr << W_ERROR "unable to allocate memory for video output stream!" << std::endl;
                        }
                    } else {
                        std::cerr << W_ERROR "unable to allocate memory for a dictionary entry!" << std::endl;
                    }
                } else {
                    std::cerr << W_ERROR "unable to allocate memory for video output context!" << std::endl;
                }
                if (errorCode == 0) {
                    // Now set up the OpenCV background subtractor object
                    gBackgroundSubtractor = createBackgroundSubtractorMOG2();
                    if (gBackgroundSubtractor) {
                        // Attach the bufferCompleted() and requestCompleted() handler
                        // functions to their events and start the camera,
                        // everything else happens in the callback functions
                        gCamera->bufferCompleted.connect(bufferCompleted);
                        gCamera->requestCompleted.connect(requestCompleted);
                        std::cout << W_INFO "starting the camera and queueing requests for 11 seconds."
                                  << std::endl;
                        gCamera->start();
                        for (std::unique_ptr<Request> &request: requests) {
                            gCamera->queueRequest(request.get());
                        }

                        std::this_thread::sleep_for(11000ms);

                        std::cout << W_INFO "stopping the camera." << std::endl;
                        gCamera->stop();
                    } else {
                        errorCode = -ENOMEM;
                        std::cerr << W_ERROR "unable to create background subtractor!" << std::endl;
                    }
                }
            }
        }

        // Tidy up
        std::cout << W_INFO "Tidying up." << std::endl;
        if (gVideoOutputContextFormat) {
            av_write_trailer(gVideoOutputContextFormat);
        }
        if (gVideoOutputFrame) {
            av_frame_free(&gVideoOutputFrame);
        }
        if (gVideoOutputContextCodec) {
            avcodec_free_context(&gVideoOutputContextCodec);
        }
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
    } else {
        std::cerr << W_ERROR "no cameras found!" << std::endl;
    }

    // Tidy up
    cm->stop();
    return (int) errorCode;
}

// End of file
