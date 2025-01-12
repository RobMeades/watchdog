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

#ifndef _W_COMMAND_LINE_H_
#define _W_COMMAND_LINE_H_

// This API is dependent on std::string.
#include <string>

/** @file
 * @brief The command-line API for the watchdog application;
 * this API is thread-safe.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Parameters passed to watchdog.
 */
typedef struct {
    std::string programName;
    std::string outputDirectory;
    std::string outputFileName;
    std::string cfgFilePath;
    bool flagStaticCamera;
    bool doNotOperateMotors;
} wCommandLineParameters_t;

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Parse the watchdog command-line for parameters.  If this
 * function returns an error and parameters is not nullptr, it will
 * populate parameters with the defaults.
 *
 * @param argc       the number of command-line parameters.
 * @param argv       the command-line parameters.
 * @param parameters a pointer to a place to store the command-
 *                   line parameters.
 * @return           zero on success, else negative error code.
 */
int wCommandLineParse(int argc, char *argv[],
                      wCommandLineParameters_t *parameters);

/** Print a set of command-line choices.
 *
 * @param choices the command-line choices.
 */
void wCommandLinePrintChoices(wCommandLineParameters_t *choices);

/** Print command-line help.
 *
 * @param defaults  a pointer to the default command-line choices.
 */
void wCommandLinePrintHelp(wCommandLineParameters_t *defaults);

#endif // _W_COMMAND_LINE_H_

// End of file
