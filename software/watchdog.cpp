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
 *
 */

#include <string>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include <libcamera/libcamera.h>

using namespace libcamera;
using namespace std::chrono_literals;

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef STREAM_ROLE_USER
// The libcamera StreamRole to use as a basis for the user stream.
# define STREAM_ROLE_USER StreamRole::VideoRecording
#endif

#ifndef STREAM_FORMAT_USER
// The pixel format for the user video stream.
# define STREAM_FORMAT_USER "XRGB8888"
#endif

#ifndef STREAM_FORMAT_HORIZONTAL_PIXELS_USER
// Horizontal size of user video stream in pixels.
# define STREAM_FORMAT_HORIZONTAL_PIXELS_USER 1920
#endif

#ifndef STREAM_FORMAT_VERTICAL_PIXELS_USER
// Vertical size of user video stream in pixels.
# define STREAM_FORMAT_VERTICAL_PIXELS_USER 1080
#endif

#ifndef STREAM_ROLE_OPENCV
// The libcamera StreamRole to use as a basis for the OpenCV stream.
# define STREAM_ROLE_OPENCV StreamRole::Viewfinder
#endif

#ifndef STREAM_FORMAT_OPENCV
// The pixel format for the OpenCV stream.
# define STREAM_FORMAT_OPENCV "RGB888"
#endif

#ifndef STREAM_FORMAT_HORIZONTAL_PIXELS_OPENCV
// Horizontal size of stream passed to OpenCV in pixels.
# define STREAM_FORMAT_HORIZONTAL_PIXELS_OPENCV 640
#endif

#ifndef STREAM_FORMAT_VERTICAL_PIXELS_OPENCV
// Vertical size of stream passed to OpenCV in pixels.
# define STREAM_FORMAT_VERTICAL_PIXELS_OPENCV 480
#endif

#ifndef FRAME_RATE
// Frames per second.
# define FRAME_RATE 30
#endif

// Where to find the two stream configurations.
#define STREAM_CFG_INDEX_USER 0
#define STREAM_CFG_INDEX_OPENCV 1

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// The camera: global as the requestComplete() callback will use it.
static std::shared_ptr<Camera> gCamera = {0};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Handle a requestComplete event from a camera.
static void requestComplete(Request *request)
{
    if (request->status() != Request::RequestCancelled) {
        // Grab a map of frame buffers by stream from the request
        const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
        for (auto bufferPair : buffers) {
            FrameBuffer *buffer = bufferPair.second;
            const FrameMetadata &metadata = buffer->metadata();
            //  For the sake of an exanple, for each buffer, print out some metadata
            std::cout << " seq: " << std::setw(6) << std::setfill('0') << metadata.sequence << " bytes used: ";
            unsigned int x = 0;
            for (const FrameMetadata::Plane &plane : metadata.planes()) {
                x++;
                std::cout << plane.bytesused;
                if (x < metadata.planes().size()) {
                    std::cout << "/";
                }
            }
            std::cout << std::endl;
        }

    }

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
    for (auto const &camera : cm->cameras()) {
        std::cout << "Found camera ID " << camera->id() << " with properties:" << std::endl;
        auto cameraProperties =  camera->properties();
        auto idMap = cameraProperties.idMap();
        for(auto &controlValue : cameraProperties) {
            auto controlId = idMap->at(controlValue.first);
            std::string value = controlValue.second.toString();
            std::cout << "  " << std::setw(6) << controlValue.first << " [" << controlId->name() << "]" << ": " << value << std::endl;
        }
    }

    // Acquire the first (and probably only) camera 
    auto cameras = cm->cameras();
    if (!cameras.empty()) {
        std::string cameraId = cameras[0]->id();
        std::cout << "Acquiring camera " << cameraId << std::endl;
        gCamera = cm->get(cameraId);
        gCamera->acquire();

        // Configure the camera with two streams: order is
        // important, so that we can pick up the given configuration
        // with a known index (index 0 is the first in the list)
        std::unique_ptr<CameraConfiguration> cameraCfg = gCamera->generateConfiguration({STREAM_ROLE_USER, STREAM_ROLE_OPENCV});
        StreamConfiguration &streamCfg = cameraCfg->at(STREAM_CFG_INDEX_USER);
        // Set it up as we'd like
        streamCfg.pixelFormat.fromString(STREAM_FORMAT_USER);
        streamCfg.size.width = STREAM_FORMAT_HORIZONTAL_PIXELS_USER;
        streamCfg.size.height = STREAM_FORMAT_VERTICAL_PIXELS_USER;
        std::cout << "Desired user stream configuration (" << STREAM_CFG_INDEX_USER << "): " << streamCfg.toString() << std::endl;

        // Then the OpenCV stream based on the view-finder (local, lowish res) role
        streamCfg = cameraCfg->at(STREAM_CFG_INDEX_OPENCV);
        // Set it up as we'd like
        streamCfg.pixelFormat.fromString(STREAM_FORMAT_OPENCV);
        streamCfg.size.width = STREAM_FORMAT_HORIZONTAL_PIXELS_OPENCV;
        streamCfg.size.height = STREAM_FORMAT_VERTICAL_PIXELS_OPENCV;
        std::cout << "Desired OpenCV stream configuration (" << STREAM_CFG_INDEX_OPENCV << "): " << streamCfg.toString() << std::endl;

        // Validate and apply the configuration
        if (cameraCfg->validate() != CameraConfiguration::Valid) {
            std::cout << "...but libcamera will adjust those values." << std::endl;
        }
        gCamera->configure(cameraCfg.get());

        std::cout << "Validated/applied camera configuration:" << std::endl;
        for (std::size_t x = 0; x < cameraCfg->size(); x++) {
            std::cout << "  " << x << ": " << cameraCfg->at(x).toString() << std::endl;
        }

        // Allocate frame buffers
        FrameBufferAllocator *allocator = new FrameBufferAllocator(gCamera);
        errorCode = 0;
        for (auto cfg = cameraCfg->begin(); (cfg != cameraCfg->end()) && (errorCode == 0); cfg++) {
            errorCode = allocator->allocate(cfg->stream());
            if (errorCode >= 0) {
                std::cout << "Allocated " << errorCode << " buffer(s) for stream " << cfg->toString() << std::endl;
                errorCode = 0;
            } else {
                std::cerr << "Unable to allocate frame buffers (error code " << errorCode << ")!" << std::endl;
            }
        }
        if (errorCode == 0) {
            std::cout << "Creating requests to the camera for each stream using the allocated buffers." << std::endl;
            // Create a queue of requests on each stream using the allocated bufffers
            std::vector<std::unique_ptr<Request>> requests;
            for (auto cfg : *cameraCfg) {
                Stream *stream = cfg.stream();
                const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);

                for (unsigned int x = 0; (x < buffers.size()) && (errorCode == 0); x++) {
                    std::unique_ptr<Request> request = gCamera->createRequest();
                    if (request) {
                        const std::unique_ptr<FrameBuffer> &buffer = buffers[x];
                        errorCode = request->addBuffer(stream, buffer.get());
                        if (errorCode == 0) {
                            requests.push_back(std::move(request));
                        } else {
                            std::cerr << "Can't attach buffer to request (" << errorCode << ")!" << std::endl;
                        }
                    } else {
                        errorCode = -ENOMEM;
                        std::cerr << "Unable to create request!" << std::endl;
                    }
                }
            }
            if (errorCode == 0) {
                // Attach the requestComplete() handler function to the request completed event of the camera
                gCamera->requestCompleted.connect(requestComplete);
                std::cout << "Starting the camera and queueing requests for 3 seconds." << std::endl;
                gCamera->start();
                for (std::unique_ptr<Request> &request : requests) {
                    gCamera->queueRequest(request.get());
                }
                std::this_thread::sleep_for(3000ms);
                std::cout << "Stopping the camera." << std::endl;
                gCamera->stop();
            }
        }

        // Tidy up
        std::cout << "Tidying up." << std::endl;
        for (auto cfg : *cameraCfg) {
            allocator->free(cfg.stream());
        }
        delete allocator;
        gCamera->release();
        gCamera.reset();
    } else {
        std::cout << "No cameras found!" << std::endl;
    }

    // Tidy up
    cm->stop();
    return (int) errorCode;
}

// End of file
