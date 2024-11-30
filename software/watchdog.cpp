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
 * Note: to run with maximum debug, execute this program as:
 *
 * LIBCAMERA_LOG_LEVELS=0 ./watchdog
 *
 * ...or to switch all debug output off:
 *
 * LIBCAMERA_LOG_LEVELS=3 ./watchdog
 *
 * The default is to run with  log level 1, which includes
 * information, warning and errors but not pure debug.
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

using namespace libcamera;
using namespace std::chrono_literals; // TODO: delete this when we don't use the delay thingy any more
using namespace cv;

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: FORMATS
 * -------------------------------------------------------------- */

#ifndef STREAM_ROLE_VIDEO_RECORDING
// The libcamera StreamRole to use as a basis for the video
// recording stream.
# define STREAM_ROLE_VIDEO_RECORDING StreamRole::VideoRecording
#endif

#ifndef STREAM_FORMAT_VIDEO_RECORDING
// The pixel format for the video recording stream.
# define STREAM_FORMAT_VIDEO_RECORDING "YUV420"
#endif

#ifndef STREAM_FORMAT_HORIZONTAL_PIXELS_VIDEO_RECORDING
// Horizontal size of video recording stream in pixels.
# define STREAM_FORMAT_HORIZONTAL_PIXELS_VIDEO_RECORDING 1920
#endif

#ifndef STREAM_FORMAT_VERTICAL_PIXELS_VIDEO_RECORDING
// Vertical size of video recording stream in pixels.
# define STREAM_FORMAT_VERTICAL_PIXELS_VIDEO_RECORDING 1080
#endif

#ifndef STREAM_ROLE_MOTION_DETECTION
// The libcamera StreamRole to use as a basis for the motion
// detection stream.
# define STREAM_ROLE_MOTION_DETECTION StreamRole::Viewfinder
#endif

#ifndef STREAM_FORMAT_MOTION_DETECTION
// The pixel format for the motion detection stream:
// though RGB888 would be immediately importable by OpenCV,
// only a Raspbarry Pi 5 is able to provide the secondary
// stream as RGB, we have to use Yxxx but that is OK:
// as sandyol poined out when I asked here:
//
// https://forums.raspberrypi.com/viewtopic.php?p=2273212#p2273212
//
// ...the Y stream is the luma information adn that can
// be passed to OpenCV as a gray-scale image, which is
// all we need for motion detection.
# define STREAM_FORMAT_MOTION_DETECTION "YUV420"
#endif

#ifndef STREAM_FORMAT_HORIZONTAL_PIXELS_MOTION_DETECTION
// Horizontal size of the stream for motion detection in pixels.
# define STREAM_FORMAT_HORIZONTAL_PIXELS_MOTION_DETECTION 854
#endif

#ifndef STREAM_FORMAT_VERTICAL_PIXELS_MOTION_DETECTION
// Vertical size of the stream for motion detection in pixels.
# define STREAM_FORMAT_VERTICAL_PIXELS_MOTION_DETECTION 480
#endif

#ifndef FRAME_RATE
// Frames per second.
# define FRAME_RATE 30
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: LOGGING
 * -------------------------------------------------------------- */

#define LOG_TAG "Watchdog"

// ANSI colour codes for printing.
#define ANSI_COLOUR_RESET "\u001b[0m"
#define ANSI_COLOUR_BRIGHT_WHITE "\u001b[37;1m"
#define ANSI_COLOUR_BRIGHT_GREEN "\u001b[32;1m"
#define ANSI_COLOUR_BRIGHT_YELLOW "\u001b[33;1m"
#define ANSI_COLOUR_BRIGHT_RED "\u001b[31;1m"

// Prefixes for info, warning and error strings.
#define INFO ANSI_COLOUR_BRIGHT_GREEN "INFO " ANSI_COLOUR_BRIGHT_WHITE LOG_TAG " " ANSI_COLOUR_RESET
#define WARN ANSI_COLOUR_BRIGHT_YELLOW "WARN " ANSI_COLOUR_BRIGHT_WHITE LOG_TAG " " ANSI_COLOUR_RESET
#define ERROR ANSI_COLOUR_BRIGHT_RED "ERROR " ANSI_COLOUR_BRIGHT_WHITE LOG_TAG " " ANSI_COLOUR_RESET

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

// The type of video stream.  Values are important here since this
// is used as an index.
typedef enum {
    WATCHDOG_STREAM_TYPE_VIDEO_RECORDING = 0,
    WATCHDOG_STREAM_TYPE_MOTION_DETECTION = 1
} watchdogStreamType_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Names for the stream types, for debug purposes.
static const char *gStreamName[] = {"video recording", "motion detection"};

// Pointer to camera: global as the requestCompleted() callback will use it.
static std::shared_ptr<Camera> gCamera = NULL;

// Pointer to the OpenCV background subtractor: global as the
// bufferCompleted() callback will use it.
static std::shared_ptr<BackgroundSubtractor> gBackgroundSubtractor = NULL;

// A place to store the foreground mask for the OpenCV stream,
// globl as the bufferCompleted() callback will populate it.
static Mat gForegroundMask;

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
                             watchdogStreamType_t streamType)
{
    return ((uint64_t) width << 48) | ((uint64_t) (height & UINT16_MAX) << 32) |
            ((stride & UINT16_MAX) << 16) | (streamType & UINT16_MAX);
}

// Decode width, height, stride and stream type from a cookie; any
// pointer parameters may be NULL.
static void cookieDecode(uint64_t cookie, unsigned int *width,
                         unsigned int *height, unsigned int *stride,
                         watchdogStreamType_t *streamType)
{
    if (width != NULL) {
        *width = (cookie >> 48) & UINT16_MAX;
    }
    if (height != NULL) {
        *height = (cookie >> 32) & UINT16_MAX;
    }
    if (stride != NULL) {
        *stride = (cookie >> 16) & UINT16_MAX;
    }
    if (streamType != NULL) {
        *streamType = (watchdogStreamType_t) (cookie & UINT16_MAX);
    }
}

// Configure a stream.
static bool streamConfigure(watchdogStreamType_t streamType,
                            StreamConfiguration &streamCfg,
                            std::string pixelFormatStr,
                            unsigned int widthPixels,
                            unsigned int heightPixels)
{
    bool formatFound = false;
    bool sizeFound = false;

    std::cout << INFO "desired " << gStreamName[streamType]
              << " stream configuration (" << streamType << "): "
              << widthPixels << "x" << heightPixels << "-"
              << pixelFormatStr << std::endl;

    // Print out the current configuration
    std::cout << INFO "existing " << gStreamName[streamType]
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
            // Found the desired format, find a size that
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
        std::cerr << ERROR "format " << pixelFormatStr << " not found, possible format(s): ";
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
            std::cerr << ERROR "size " << widthPixels << "x" << heightPixels
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
        std::cout << INFO "nearest " << gStreamName[streamType]
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

        // Convert a libcamera FrameBuffer into an OpenCV Mat
        int fd = buffer->planes()[0].fd.get();
        // Grab the stream's width, height, stride and which
        // stream this is, all of which is encoded in the
        // buffer's cookie when we associated it with the stream
        unsigned int width;
        unsigned int height;
        unsigned int stride;
        watchdogStreamType_t streamType;
        cookieDecode(buffer->cookie(), &width, &height, &stride, &streamType);
        // From this post: https://forums.raspberrypi.com/viewtopic.php?t=347925
        // need to create a memory map into the frame buffer for OpenCV
        // to be able to access it
        uint8_t *frameMapped = static_cast<uint8_t *> (mmap(NULL, buffer->planes()[0].length,
                                                            PROT_READ | PROT_WRITE, MAP_SHARED,
                                                            fd, 0));
        // From the comment on this post:
        // https://stackoverflow.com/questions/44517828/transform-a-yuv420p-qvideoframe-into-grayscale-opencv-mat
        // ...we can bring in just the Y portion of the frame as effectively
        // a gray-scale image using CV_8UC1
        Mat frameOpenCv(height, width, CV_8UC1, frameMapped, stride);

        // Set JPEG image quality for OpenCV file write
        std::vector<int> imageCompression;
        imageCompression.push_back(IMWRITE_JPEG_QUALITY);
        imageCompression.push_back(40);

        // Now do the OpenCV things
        if (!frameOpenCv.empty()) {
           std::string sequence = std::to_string(metadata.sequence);
           if (streamType == WATCHDOG_STREAM_TYPE_MOTION_DETECTION) { 
                // Update the background model of the motion detection stream
                //gBackgroundSubtractor->apply(frameOpenCv, gForegroundMask);
                std::string fileName = "lores" + sequence + ".jpg";
                imwrite(fileName, frameOpenCv, imageCompression);
           } else {
                // Just to show that we've had an effect on the world,
                // write something on the current frame
                rectangle(frameOpenCv, cv::Point(10, 2), cv::Point(100,20),
                          cv::Scalar(255,255,255), -1);
                putText(frameOpenCv, sequence, cv::Point(15, 15),
                        FONT_HERSHEY_SIMPLEX, 0.5 , cv::Scalar(0,0,0));
                // Write the image to file
                std::string fileName = "main" + sequence + ".jpg";
                imwrite(fileName, frameOpenCv, imageCompression);
            }
        }

        //  For the sake of an example, for each buffer, print out some metadata
        std::cout << INFO "seq: " << std::setw(6) << std::setfill('0')
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
        std::cout << INFO "found camera ID " << camera->id()
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
        std::cout << INFO "acquiring camera " << cameraId << std::endl;
        gCamera = cm->get(cameraId);
        gCamera->acquire();

        // Configure the camera with two streams: order is
        // important, so that we can pick up the given configuration
        // with a known index (index 0 is the first in the list)
        std::unique_ptr<CameraConfiguration> cameraCfg = gCamera->generateConfiguration({STREAM_ROLE_VIDEO_RECORDING,
                                                                                         STREAM_ROLE_MOTION_DETECTION});
        // First the video recording stream
        streamConfigure(WATCHDOG_STREAM_TYPE_VIDEO_RECORDING,
                        cameraCfg->at(WATCHDOG_STREAM_TYPE_VIDEO_RECORDING),
                        STREAM_FORMAT_VIDEO_RECORDING,
                        STREAM_FORMAT_HORIZONTAL_PIXELS_VIDEO_RECORDING,
                        STREAM_FORMAT_VERTICAL_PIXELS_VIDEO_RECORDING);
        // Then the motion detection stream
        streamConfigure(WATCHDOG_STREAM_TYPE_MOTION_DETECTION,
                        cameraCfg->at(WATCHDOG_STREAM_TYPE_MOTION_DETECTION),
                        STREAM_FORMAT_MOTION_DETECTION,
                        STREAM_FORMAT_HORIZONTAL_PIXELS_MOTION_DETECTION,
                        STREAM_FORMAT_VERTICAL_PIXELS_MOTION_DETECTION);

        // Validate and apply the configuration
        if (cameraCfg->validate() != CameraConfiguration::Valid) {
            std::cout << WARN "libcamera will adjust those values." << std::endl;
        }
        gCamera->configure(cameraCfg.get());

        std::cout << INFO "validated/applied camera configuration:" << std::endl;
        for (std::size_t x = 0; x < cameraCfg->size(); x++) {
            std::cout << "  " << x << ": " << cameraCfg->at(x).toString() << std::endl;
        }

        // Allocate frame buffers
        FrameBufferAllocator *allocator = new FrameBufferAllocator(gCamera);
        errorCode = 0;
        for (auto cfg = cameraCfg->begin(); (cfg != cameraCfg->end()) && (errorCode == 0); cfg++) {
            errorCode = allocator->allocate(cfg->stream());
            if (errorCode >= 0) {
                std::cout << INFO "allocated " << errorCode << " buffer(s) for stream "
                          << cfg->toString() << std::endl;
                errorCode = 0;
            } else {
                std::cerr << ERROR "unable to allocate frame buffers (error code "
                          << errorCode << ")!" << std::endl;
            }
        }
        if (errorCode == 0) {
            std::cout << INFO "creating requests to the camera for each stream using the allocated buffers."
                      << std::endl;
            // Create a queue of requests on each stream using the allocated bufffers
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
                            // Encode the width, height, stride and index of the stream
                            // into the cookie of the FrameBuffer as we will need
                            // that information later when converting the FrameBuffer
                            // to a form that OpenCV understands
                            buffer->setCookie(cookieEncode(stream->configuration().size.width,
                                                           stream->configuration().size.height,
                                                           stream->configuration().stride,
                                                           (watchdogStreamType_t) streamIndex));
                            requests.push_back(std::move(request));
                        } else {
                            std::cerr << ERROR "can't attach buffer to request (" << errorCode << ")!"
                                      << std::endl;
                        }
                    } else {
                        errorCode = -ENOMEM;
                        std::cerr << ERROR "unable to create request!" << std::endl;
                    }
                }
                streamIndex++;
            }
            if (errorCode == 0) {
                // That's got pretty much all of the libcamera stuff, the camera
                // setup, done.  Now set up the OpenCV processing we need.
                // First, create the background subtractor object
                gBackgroundSubtractor = createBackgroundSubtractorMOG2();
                if (gBackgroundSubtractor) {
                    // Attach the bufferCompleted() and requestCompleted() handler
                    // functions to their events
                    gCamera->bufferCompleted.connect(bufferCompleted);
                    gCamera->requestCompleted.connect(requestCompleted);
                    std::cout << INFO "starting the camera and queueing requests for 3 seconds."
                              << std::endl;
                    gCamera->start();
                    for (std::unique_ptr<Request> &request: requests) {
                        gCamera->queueRequest(request.get());
                    }
                    std::this_thread::sleep_for(3000ms);
                    std::cout << INFO "stopping the camera." << std::endl;
                    gCamera->stop();
                } else {
                    errorCode = -ENOMEM;
                    std::cerr << ERROR "unable to create background subtractor!" << std::endl;
                }
            }
        }

        // Tidy up
        std::cout << INFO "Tidying up." << std::endl;
        for (auto cfg: *cameraCfg) {
            allocator->free(cfg.stream());
        }
        delete allocator;
        gCamera->release();
        gCamera.reset();
    } else {
        std::cerr << ERROR "no cameras found!" << std::endl;
    }

    // Tidy up
    cm->stop();
    return (int) errorCode;
}

// End of file
