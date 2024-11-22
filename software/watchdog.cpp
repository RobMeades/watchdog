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

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// The camera.
static std::shared_ptr<Camera> camera;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */


/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// The entry point.
int main()
{
    int errorCode = -1;

    // Create and start a camera manager instance
    std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
    cm->start();

    // List the available cameras
    int count = 0;
    for (auto const &camera : cm->cameras()) {
        std::cout << "Camera ID: " << camera->id() << std::endl;
        count++;
    }
    printf("Found %d camera(s).\n", count);
    if (count > 0) {
        errorCode = 0;
    }

    cm->stop();
    return errorCode;
}

// End of file
