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
 * @brief The watchdog application, main().
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
 *   is different to the other Pis where GPIOs are concerned, see
 *   http://git.munts.com/muntsos/doc/AppNote11-link-gpiochip.pdf.
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
 * The default is to run with log level 1, which includes
 * information, warning and errors from libcamera, but not pure debug.
 */

// The Linux/Posix stuff.
#include <unistd.h> // For sleep()

// The watchdog stuff.
#include <w_util.h>
#include <w_log.h>
#include <w_command_line.h>
#include <w_cfg.h>
#include <w_gpio.h>
#include <w_msg.h>
#include <w_control.h>
#include <w_led.h>
#include <w_motor.h>
#include <w_hls.h>
#include <w_camera.h>
#include <w_image_processing.h>
#include <w_video_encode.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// The entry point.
int main(int argc, char *argv[])
{
    int errorCode = -ENXIO;
    wCommandLineParameters_t commandLineParameters = {};

    // Process the command-line parameters
    if (wCommandLineParse(argc, argv, &commandLineParameters) == 0) {
        wCommandLinePrintChoices(&commandLineParameters);
        // Capture CTRL-C so that we can exit in an organised fashion
        wUtilTerminationCaptureSet();

        // Initialise configuration
        errorCode = wCfgInit(commandLineParameters.cfgFilePath);
        if (errorCode == 0) {
            // Initialise GPIOs
            errorCode = wGpioInit();
        }
        if (errorCode == 0) {
            // Initialise the motors and set any user rest/range values
            errorCode = wMotorInit(commandLineParameters.doNotOperateMotors);
            if (errorCode == 0) {
                errorCode = wMotorRestSet(W_MOTOR_TYPE_VERTICAL,
                                          commandLineParameters.restVerticalSteps);
            }
            if (errorCode == 0) {
                wMotorMoveToRest(W_MOTOR_TYPE_VERTICAL);
                errorCode = wMotorRestSet(W_MOTOR_TYPE_ROTATE,
                                          commandLineParameters.restHorizontalSteps);
            }
            if (errorCode == 0) {
                wMotorMoveToRest(W_MOTOR_TYPE_ROTATE);
                errorCode = wMotorRangeSet(W_MOTOR_TYPE_VERTICAL,
                                           commandLineParameters.lookUpLimitSteps,
                                           commandLineParameters.lookDownLimitSteps);
            }
            if (errorCode == 0) {
                errorCode = wMotorRangeSet(W_MOTOR_TYPE_ROTATE,
                                           commandLineParameters.lookRightLimitSteps,
                                           commandLineParameters.lookLeftLimitSteps);
            }
        }
        if (errorCode == 0) {
            // Initialise messaging
            errorCode = wMsgInit();
        }
        if (errorCode == 0) {
            // Now that messaging is available, we should
            // be able to initialise control
            errorCode = wControlInit();
        }
        if (errorCode == 0) {
            // Intialise the LEDs
            errorCode = wLedInit();
        }
        if (errorCode == 0) {
            // List the cameras and then initialise the camera
            wCameraList();
            errorCode = wCameraInit();
        }
        if (errorCode == 0) {
            // Now that the camera has been initialised, we
            // should be able to initialise image processing
            errorCode = wImageProcessingInit();
        }
        if (errorCode == 0) {
            // Remove any old files for a clean start
            system(std::string("rm " +
                               commandLineParameters.outputDirectory +
                               W_UTIL_DIR_SEPARATOR +
                               commandLineParameters.outputFileName +
                               W_HLS_PLAYLIST_FILE_EXTENSION +
                               W_UTIL_SYSTEM_SILENT).c_str());
            system(std::string("rm " +
                               commandLineParameters.outputDirectory +
                               W_UTIL_DIR_SEPARATOR +
                               commandLineParameters.outputFileName +
                               "*" W_HLS_SEGMENT_FILE_EXTENSION +
                               W_UTIL_SYSTEM_SILENT).c_str());

            // Make sure the output directory exists
            system(std::string("mkdir -p " +
                               commandLineParameters.outputDirectory).c_str());

            // With image processing initialised, we should
            // be able to initialise video encoding
            errorCode = wVideoEncodeInit(commandLineParameters.outputDirectory,
                                         commandLineParameters.outputFileName);
        }

        if (errorCode == 0) {
            // Everything is now initialised, ready to go; kick things off
            // by starting control operations, which will request the video
            // encode to start encoding, which will in turn start the image
            // processing code, which will in turn start the camera
            errorCode = wControlStart(commandLineParameters.flagStaticCamera,
                                      commandLineParameters.motionContinuousSeconds);

            W_LOG_INFO("running, press CTRL-C to stop.");
            while ((errorCode == 0) && wUtilKeepGoing()) {
                sleep(1);
            }

            // Done
            wControlStop();
        } else {
            W_LOG_ERROR("initialisation failure (%d)!", errorCode);
        }

        wVideoEncodeDeinit();
        wImageProcessingDeinit();
        wCameraDeinit();
        wLedDeinit();
        wControlDeinit();
        wMsgDeinit();
        wMotorDeinit();
        wGpioDeinit();
        wCfgDeinit();

        W_LOG_INFO_START("exiting");
        if (errorCode != 0) {
            W_LOG_INFO(" with error code %d", errorCode);
        }
        W_LOG_INFO_MORE(".");
        W_LOG_INFO_END;

    } else {
        // Print help about the command line, including the defaults
        wCommandLinePrintHelp(&commandLineParameters);
    }

    return errorCode;
}

// End of file
