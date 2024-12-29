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
 * @brief Implementation of the command-line functions (parsing, printing
 * help. etc.) for the watchdog.
 */

// The CPP stuff.
#include <string>
#include <iomanip>
#include <iostream>

// The Linux/Posix stuff.
#include <unistd.h> // For get_current_dir_name()

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>
#include <w_hls.h>

// Us.
#include <w_command_line.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS:
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Given a C string that is assumed to be a path, return the directory
// portion of that as a C++ string.
static std::string getDirectoryPath(const char *path, bool absolute=false)
{
    std::string directoryPath;

    if (path) {
        directoryPath = std::string(path);
        if (absolute && !(directoryPath.find_first_of(W_UTIL_DIR_SEPARATOR) == 0)) {
            // If we haven't already got an absolute path, make it absolute
            char *currentDirName = get_current_dir_name();
            if (currentDirName) {
                directoryPath = std::string(currentDirName) + W_UTIL_DIR_SEPARATOR + directoryPath;
                free(currentDirName);
            } else {
                W_LOG_ERROR("unable to get the current directory name");
            }
        }
        // Remove any slash off the end to avoid double-slashing when
        // we concatenate this with something else
        unsigned int length = directoryPath.length();
        if ((length > 0) && (directoryPath.find_last_of(W_UTIL_DIR_SEPARATOR) == length)) {
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

    std::cout << "  -d <directory path> set directory for streaming output"
              << " (default ";
    if (defaults && (defaults->outputDirectory != std::string(W_UTIL_DIR_THIS))) {
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

// End of file
