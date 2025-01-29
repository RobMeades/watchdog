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
 * @brief The implementation of the command-line functions (parsing,
 * printing help. etc.) for the watchdog application.
 */

// The CPP stuff.
#include <string>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>
#include <w_hls.h>
#include <w_cfg.h>

// Us.
#include <w_command_line.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS:
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Given a C string that is assumed to be a path, return the file name
// portion of that string.
static std::string getFileName(const char *path)
{
    std::string fileName;

    if (path) {
        fileName = std::string(path);
        // Skip past any directory separators
        unsigned int pos = fileName.find_last_of(W_UTIL_DIR_SEPARATOR);
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

// Get a positive integer value from a command-line parameter.
static int getPositiveInteger(std::string str)
{
    int valueOrErrorCode = -EINVAL;

    if (!str.empty()) {
        try {
            int x = std::stoi(str);
            if (x >= 0) {
                valueOrErrorCode = x;
            }
        } catch (int error) {
            valueOrErrorCode = -error;
        }
    }

    return valueOrErrorCode;
}

// Get a signed integer value from a command-line parameter.
static int getInteger(std::string str, int *value)
{
    int errorCode = -EINVAL;

    if (value && !str.empty()) {
        try {
            *value = std::stoi(str);
            errorCode = 0;
        } catch (int error) {
            errorCode = -error;
        }
    }

    return errorCode;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Process the command-line parameters.  If this function returns
// an error and parameters is not nullptr, it will populate
// parameters with the defaults.
int wCommandLineParse(int argc, char *argv[],
                      wCommandLineParameters_t *parameters)
{
    int errorCode = -EINVAL;
    int x = 0;

    if (parameters) {
        parameters->programName = std::string(W_HLS_FILE_NAME_ROOT_DEFAULT);
        parameters->outputDirectory = std::string(W_HLS_OUTPUT_DIRECTORY_DEFAULT);
        parameters->outputFileName = std::string(W_HLS_FILE_NAME_ROOT_DEFAULT);
        parameters->cfgFilePath = std::string(W_CFG_FILE_PATH_DEFAULT);
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
                        std::string str = std::string(argv[x]);
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
                // Test for configuration file path option
                } else if (std::string(argv[x]) == "-c") {
                    x++;
                    if (x < argc) {
                        errorCode = 0;
                        std::string str = std::string(argv[x]);
                        if (!str.empty()) {
                            parameters->cfgFilePath = str;
                        }
                    }
                // Test for the continuous motion option
                } else if (std::string(argv[x]) == "-m") {
                    x++;
                    if (x < argc) {
                        errorCode = getPositiveInteger(std::string(argv[x]));
                        if (errorCode >= 0) {
                            parameters->motionContinuousSeconds = errorCode;
                            errorCode = 0;
                        }
                    }
                // Test for vertical rest
                } else if (std::string(argv[x]) == "-rv") {
                    x++;
                    if (x < argc) {
                        errorCode = getInteger(std::string(argv[x]),
                                               &(parameters->restVerticalSteps));
                    }
                // Test for horizontal rest option
                } else if (std::string(argv[x]) == "-rh") {
                    x++;
                    if (x < argc) {
                        errorCode = getInteger(std::string(argv[x]),
                                               &(parameters->restHorizontalSteps));
                    }
                // Test for look-up limit option
                } else if (std::string(argv[x]) == "-lu") {
                    x++;
                    if (x < argc) {
                        errorCode = getInteger(std::string(argv[x]),
                                               &(parameters->lookUpLimitSteps));
                    }
                // Test for look-down limit option
                } else if (std::string(argv[x]) == "-ld") {
                    x++;
                    if (x < argc) {
                        errorCode = getInteger(std::string(argv[x]),
                                               &(parameters->lookDownLimitSteps));
                    }
                // Test for look-right limit option
                } else if (std::string(argv[x]) == "-lr") {
                    x++;
                    if (x < argc) {
                        errorCode = getInteger(std::string(argv[x]),
                                               &(parameters->lookRightLimitSteps));
                    }
                // Test for look-left limit option
                } else if (std::string(argv[x]) == "-ll") {
                    x++;
                    if (x < argc) {
                        errorCode = getInteger(std::string(argv[x]),
                                               &(parameters->lookLeftLimitSteps));
                    }
                // Test for flagStaticCamera
                } else if (std::string(argv[x]) == "-s") {
                    parameters->flagStaticCamera = true;
                    errorCode = 0;
                // Test for doNotOperateMotors
                } else if (std::string(argv[x]) == "-z") {
                    parameters->doNotOperateMotors = true;
                    errorCode = 0;
                }
                x++;
            }
        }
    }

    return errorCode;
}

// Print command-line choices.
void wCommandLinePrintChoices(wCommandLineParameters_t *choices)
{
    std::string programName = W_HLS_FILE_NAME_ROOT_DEFAULT;

    if (choices && !choices->programName.empty()) {
        programName = choices->programName;
    }
    std::cout << programName;
    if (choices) {
        std::cout << ", putting output files ("
                  << W_HLS_PLAYLIST_FILE_EXTENSION << " and "
                  << W_HLS_SEGMENT_FILE_EXTENSION << ") in ";
        if (choices->outputDirectory != std::string(W_UTIL_DIR_THIS)) {
            std::cout << choices->outputDirectory;
        } else {
            std::cout << "this directory";
        }
        std::cout << ", output files will be named "
                  << choices->outputFileName;
        std::cout << ", the JSON configuration file will be "
                  << choices->cfgFilePath;
        if (choices->motionContinuousSeconds > 0) {
            std::cout << ", continuous motion is required for "
                      <<  choices->motionContinuousSeconds
                      << " second(s)";
        }
        if (choices->restVerticalSteps > 0) {
            std::cout << ", vertical rest position is "
                      <<  choices->restVerticalSteps
                      << " steps(s) relative to centre";
        }
        if (choices->restHorizontalSteps > 0) {
            std::cout << ", horizontal rest position is "
                      <<  choices->restHorizontalSteps
                      << " steps(s) relative to centre";
        }
        if (choices->lookUpLimitSteps > 0) {
            std::cout << ", look-up limit is "
                      <<  choices->lookUpLimitSteps
                      << " steps(s) relative to centre";
        }
        if (choices->lookDownLimitSteps > 0) {
            std::cout << ", look-down limit is "
                      <<  choices->lookDownLimitSteps
                      << " steps(s) relative to centre";
        }
        if (choices->lookRightLimitSteps > 0) {
            std::cout << ", look-right limit is "
                      <<  choices->lookRightLimitSteps
                      << " steps(s) relative to centre";
        }
        if (choices->lookLeftLimitSteps > 0) {
            std::cout << ", look-left limit is "
                      <<  choices->lookLeftLimitSteps
                      << " steps(s) relative to centre";
        }
        if (choices->flagStaticCamera) {
            std::cout << ", head will not track";
        }
        if (choices->doNotOperateMotors) {
            std::cout << ", motors will not move";
        }
    }
    std::cout << "." << std::endl;
}

// Print command-line help.
void wCommandLinePrintHelp(wCommandLineParameters_t *defaults)
{
    std::string programName = W_HLS_FILE_NAME_ROOT_DEFAULT;

    if (defaults && !defaults->programName.empty()) {
        programName = defaults->programName;
    }
    std::cout << programName << ", options are:" << std::endl;

    std::cout << "  -d  <directory path> set directory for streaming output"
              << " (default ";
    if (defaults && (defaults->outputDirectory != std::string(W_UTIL_DIR_THIS))) {
        std::cout << defaults->outputDirectory;
    } else {
        std::cout << "this directory";
    }
    std::cout << ")." << std::endl;

    std::cout << "  -f  <file name> set file name for streaming output ("
              <<  W_HLS_PLAYLIST_FILE_EXTENSION << " and "
              <<  W_HLS_SEGMENT_FILE_EXTENSION << " files)";
    if (defaults && !defaults->outputFileName.empty()) {
        std::cout << " (default " << defaults->outputFileName << ")";
    }
    std::cout << "." << std::endl;

    std::cout << "  -c  <file path> set file path of the JSON configuration"
              << " file used by the web interface (or anything else for that"
              << " matter) to control behaviour (default ";
    if (defaults && !defaults->cfgFilePath.empty()) {
        std::cout << defaults->cfgFilePath;
    } else {
        std::cout << "no configuration file will be used";
    }
    std::cout << ");" << std::endl;
    std::cout << "      if the file does not exist a default file containing"
              << " all possible options will be written." << std::endl;

    std::cout << "  -m  <integer> motion must have been occurring for this number"
              << " of seconds before the watchdog will react to it (default ";
    if (defaults && (defaults->motionContinuousSeconds > 0)) {
        std::cout << defaults->motionContinuousSeconds;
        std::cout << " second(s)";
    } else {
        std::cout << "zero";
    }
    std::cout << ")." << std::endl;

    std::cout << "  -rx <integer>, where x is v or h: override the rest position,"
              << " either vertically or horizontally in steps;" << std::endl;
    std::cout << "      values can be positive or negative, relative to the centre"
              << " of the calibrated range for the given axis (default";
    if (!defaults || ((defaults->restVerticalSteps == 0) &&
                      (defaults->restHorizontalSteps == 0))) {
        std::cout << " no override";
    } else {
        if (defaults) {
            std::cout << "s "
                      << defaults->restVerticalSteps
                      << ", and "
                      << defaults->restHorizontalSteps
                      << " respectively";
        } else {
            std::cout << " ??";
        }
    }
    std::cout << ")." << std::endl;

    std::cout << "  -lx <integer>, where x is u, d, r or l: override the"
              << " limit for looking up, down, right or left to this"
              << " number of steps;" << std::endl;
    std::cout << "      down/left values would normally be negative, up/right values positive,"
              << " relative to the calibrated centre for the given axis (default";
    if (!defaults || ((defaults->lookUpLimitSteps == 0) &&
                      (defaults->lookDownLimitSteps == 0) &&
                      (defaults->lookRightLimitSteps == 0) &&
                      (defaults->lookLeftLimitSteps == 0))) {
        std::cout << " no override";
    } else {
        if (defaults) {
            std::cout << "s "
                      << defaults->lookUpLimitSteps
                      << ", "
                      << defaults->lookDownLimitSteps
                      << ", "
                      << defaults->lookRightLimitSteps
                      << ", and "
                      << defaults->lookLeftLimitSteps
                      << " respectively";
        } else {
            std::cout << " ??";
        }
    }
    std::cout << ")." << std::endl;

    std::cout << "  -s  static camera (head will move for calibration but not thereafter)";
    if (defaults) {
        std::cout << " (default " << (defaults->flagStaticCamera ? "on)" : "off)");
    }
    std::cout << "." << std::endl;

    std::cout << "  -z  do not operate motors (used for deubg/maintenance only)";
    if (defaults) {
        std::cout << " (default " << (defaults->doNotOperateMotors ? "on)" : "off)");
    }
    std::cout << "." << std::endl;

    std::cout << "Note that this program needs to be able to access HW and";
    std::cout << " change scheduling priority, which requires elevated privileges." << std::endl;
}

// End of file
