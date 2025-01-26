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
 * @brief The implementation of the GPIO portion of the watchdog application.
 *
 * This code makes use of libcamera to read/write GPIO pins, hence must
 * be linked with libcamera.
 *
 * Note: to run with maximum debug from libcamera, execute the program
 * this forms part of (e.g. watchdog) with:
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

// The Linux/Posix stuff.
#include <sys/mman.h>
#include <fcntl.h>

// The libcamera stuff.
#include <libcamera/libcamera.h>

// Other parts of watchdog.
#include <w_common.h>
#include <w_util.h>
#include <w_log.h>
#include <w_msg.h>
#include <w_image_processing.h>

// Us.
#include <w_camera.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_CAMERA_STREAM_ROLE
// The libcamera StreamRole to use as a basis for the video stream.
# define W_CAMERA_STREAM_ROLE libcamera::StreamRole::VideoRecording
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Context needed by the camera stuff here.
 */
typedef struct {
    std::shared_ptr<libcamera::Camera> camera;
    std::unique_ptr<libcamera::CameraManager> cameraManager;
    std::unique_ptr<libcamera::CameraConfiguration> cameraCfg;
    libcamera::FrameBufferAllocator *allocator;
    std::vector<std::unique_ptr<libcamera::Request>> requests;
    libcamera::ControlList cameraControls;
    wCommonFrameFunction_t *outputCallback;
    uint64_t frameCount;
} wCameraContext_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Our context.
static wCameraContext_t *gContext = nullptr;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MISC
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

// Close stuff and release memory.
static void cleanUp()
{
    if (gContext) {
        if (gContext->camera) {
            gContext->camera->stop();
        }

        if (gContext->cameraCfg && gContext->allocator) {
            for (auto cfg: *(gContext->cameraCfg)) {
                gContext->allocator->free(cfg.stream());
            }
            delete gContext->allocator;
        }

        if (gContext->camera) {
            gContext->camera->release();
            gContext->camera.reset();
        }

        if (gContext->cameraManager) {
            gContext->cameraManager->stop();
        }

        delete gContext;
        gContext = nullptr;
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: LIBCAMERA CALLBACK
 * -------------------------------------------------------------- */

// Handle a requestCompleted event from a camera.
static void requestCompleted(libcamera::Request *request)
{
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
                if (gContext->outputCallback) {
                    // Pass a copy of the camera frame to the image processing callback
                    uint8_t *data = (uint8_t *) malloc(dmaBufferLength);
                    if (data) {
                        memcpy(data, dmaBuffer, dmaBufferLength);
                        gContext->outputCallback(data, dmaBufferLength,
                                                 metadata.sequence,
                                                 width, height, stride);
                    } else {
                        W_LOG_ERROR("unable to allocate %d byte(s) for image buffer,"
                                    " a frame has been lost.", dmaBufferLength);
                    }
                }
                // Done with the mapping now
                munmap(dmaBuffer, dmaBufferLength);
            } else {
                W_LOG_ERROR("mmap() returned error %d, a frame has been lost.",
                            errno);
            }

            gContext->frameCount++;

            // Re-use the request
            request->reuse(libcamera::Request::ReuseBuffers);
            gContext->camera->queueRequest(request);
        }
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise the camera.
int wCameraInit()
{
    int errorCode = 0;

    if (!gContext) {
        gContext = new wCameraContext_t;
        errorCode = -ENXIO;

        // Create and start a camera manager instance
        gContext->cameraManager = std::make_unique<libcamera::CameraManager>();
        gContext->cameraManager->start();

       // Acquire the first (and probably only) camera
        auto cameras = gContext->cameraManager->cameras();
        if (!cameras.empty()) {
            errorCode = 0;
            std::string cameraId = cameras[0]->id();
            W_LOG_INFO("acquiring camera %s.", cameraId.c_str());
            std::shared_ptr<libcamera::Camera> camera = gContext->cameraManager->get(cameraId);
            gContext->camera = camera;
            camera->acquire();

            // Configure the camera with the stream
            gContext->cameraCfg = camera->generateConfiguration({W_CAMERA_STREAM_ROLE});
            cameraStreamConfigure(gContext->cameraCfg->at(0), W_CAMERA_STREAM_FORMAT,
                                  W_CAMERA_WIDTH_PIXELS,
                                  W_CAMERA_HEIGHT_PIXELS);

#if W_CAMERA_ROTATED_180
            gContext->cameraCfg->orientation = libcamera::Orientation::Rotate180;
#endif

            // Validate and apply the configuration
            if (gContext->cameraCfg->validate() != libcamera::CameraConfiguration::Valid) {
                W_LOG_DEBUG("libcamera will adjust those values.");
            }
            camera->configure(gContext->cameraCfg.get());

            W_LOG_INFO_START("validated/applied camera configuration: ");
            for (std::size_t x = 0; x < gContext->cameraCfg->size(); x++) {
                if (x > 0) {
                    W_LOG_INFO_MORE(", ");
                }
                W_LOG_INFO_MORE("%s", gContext->cameraCfg->at(x).toString().c_str());
                x++;
            }
            W_LOG_INFO_MORE(".");
            W_LOG_INFO_END;

            // Allocate frame buffers
            libcamera::FrameBufferAllocator *allocator;
            try {
                allocator = new libcamera::FrameBufferAllocator(camera);
            }
            catch (int x) {
                errorCode = -x;
            }
            if (errorCode == 0) {
                gContext->allocator = allocator;
                for (auto cfg = gContext->cameraCfg->begin();
                     (cfg != gContext->cameraCfg->end()) && (errorCode == 0);
                     cfg++) {
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
            }
            if (errorCode == 0) {
                W_LOG_DEBUG("creating requests to the camera using the allocated buffers.");
                // Create a queue of requests using the allocated buffers
                for (auto cfg: *gContext->cameraCfg) {
                    libcamera::Stream *stream = cfg.stream();
                    const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers = allocator->buffers(stream);
                    for (unsigned int x = 0; (x < buffers.size()) && (errorCode == 0); x++) {
                        std::unique_ptr<libcamera::Request> request = camera->createRequest();
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
                                gContext->requests.push_back(std::move(request));
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
            }

            if (errorCode == 0) {
                // We have not yet set any of the controls for the camera;
                // the only one we care about here is the frame rate,
                // so that the settings above match.  There is a minimum
                // and a maximum, setting both the same fixes the rate.
                // We create a camera control list and pass it to
                // the start() method when we start the camera.
                // Units are microseconds.
                int64_t frameDurationLimit = 1000000 / W_CAMERA_FRAME_RATE_HERTZ;
                gContext->cameraControls.set(libcamera::controls::FrameDurationLimits,
                                             libcamera::Span<const std::int64_t, 2>({frameDurationLimit,
                                                                                     frameDurationLimit}));
            }
        } else {
            W_LOG_ERROR("no cameras found!");
        }

        if (errorCode != 0) {
            cleanUp();
        }
    }

    return errorCode;
}

// Start the camera.
int wCameraStart(wCommonFrameFunction_t *outputCallback)
{
    int errorCode = -EBADF;

    if (gContext) {
        gContext->outputCallback = outputCallback;
        std::shared_ptr<libcamera::Camera> camera = gContext->camera;

        // Attach the requestCompleted() handler
        // function to its events and start the camera;
        // everything else happens in requestCompleted()
        camera->requestCompleted.connect(requestCompleted);

        // Pedal to da metal
        W_LOG_INFO("starting the camera and queueing requests.");
        camera->start(&(gContext->cameraControls));
        for (std::unique_ptr<libcamera::Request> &request: gContext->requests) {
            camera->queueRequest(request.get());
        }
        errorCode = 0;
    }

    return errorCode;
}

// Get the current frame count of the camera.
uint64_t wCameraFrameCountGet()
{
    uint64_t frameCount = 0;

    if (gContext) {
        frameCount = gContext->frameCount;
    }

    return frameCount;
}

// Stop the camera.
int wCameraStop()
{
    int errorCode = -EBADF;

    if (gContext) {
        W_LOG_INFO("stopping the camera.");
        gContext->camera->stop();
        gContext->outputCallback = nullptr;
    }

    return errorCode;
}

// Deinitialise the camera.
void wCameraDeinit()
{
    if (gContext) {
        cleanUp();
    }
}

// List the available cameras and their properties.
int wCameraList()
{
    int errorCode = -EBADF;

    if (!gContext) {
        errorCode = 0;
        // Create and start a camera manager instance
        std::unique_ptr<libcamera::CameraManager> cm = std::make_unique<libcamera::CameraManager>();
        cm->start();

        // List the available cameras
        for (auto const &camera: cm->cameras()) {
            errorCode++;
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

        if (errorCode <= 0) {
            W_LOG_INFO("found no cameras.");
        }

        cm->stop();
    } else {
        W_LOG_ERROR("cannot scan for cameras while initialised!");
    }

    return errorCode;
}

// End of file
