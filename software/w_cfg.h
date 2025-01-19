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

#ifndef _W_CFG_H_
#define _W_CFG_H_

// This API is dependent on std::string and w_util.h.
#include <string>
#include <w_util.h>

/** @file
 * @brief The configuration API for the watchdog application; these
 * functions are thread-safe aside from gCfgInit() and gCfgDeinit(),
 * which should not be called at the same time as any other of these
 * APIs ore each other.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_CFG_FILE_NAME_DEFAULT
/** The default file name for the configuration file.
 */
# define W_CFG_FILE_NAME_DEFAULT "watchdog"
#endif

#ifndef W_CFG_FILE_EXTENSION
/** Configuration file extension.
 */
# define W_CFG_FILE_EXTENSION ".cfg"
#endif

#ifndef W_CFG_FILE_PATH_DEFAULT
/** The default configuration file path (i.e. path plus file name).
 */
# define W_CFG_FILE_PATH_DEFAULT W_UTIL_DIR_THIS W_UTIL_DIR_SEPARATOR W_CFG_FILE_NAME_DEFAULT W_CFG_FILE_EXTENSION
#endif

#ifndef W_CFG_FILE_DEFAULT
/** The default contents of a configuration file.
 *
 * The "override" field can be used to stop or start "motors" and/or
 * "lights" until a given time.  The format for "offUntil" and
 * "onUntil" is ISO8601 except that anything smaller than seconds,
 * and any marker on the end, if present, will be ignored.
 * Only the first "motors"/"lights" item, and the first "offUntil"
 * or "onUntil" item within each, matters (duplicates, and an "offUntil"
 * if there is an "onUntil", and vice-versa, will be ignored).
 * The default configuration file contents below are intended to show
 * the format but don't do anything of use, i.e. the lights and motors
 * will remain on.
 *
 * "week" represents a weekly schedule (overridden by the standalone
 * "motors" and "lights" fields), in which the time format is
 * HH:MM:SS and all times are local.  Motors and lights are assumed
 * to be on at midnight on Sunday and may be switched on or off
 * thereafter. Days may be omitted, items within days may be
 * omitted (provided they form valid JSON), all this program does
 * is check if a time of the week has passed and applies the
 * configuration. In the default configuration file contents below
 * an "on" field is included in entries simply so as to form valid JSON;
 * an "on"s has no effect on the world if that thing is already on.
 *
 * In all cases, duplicate keys are ignored (only the first is treated).
 *
 * wCfgInit() will fail if this is not valid JSON and no alternative
 * [valid] default JSON is passed to it.
 */
# define W_CFG_FILE_DEFAULT "\
{\n\
    \"override\": {\n\
        \"motors\": {\n\
            \"offUntil\": \"2025-01-10T23:07:55\",\n\
            \"onUntil\": \"2025-01-10T23:07:55\"\n\
        },\n\
        \"lights\": {\n\
            \"offUntil\": \"2025-01-10T23:07:55\",\n\
            \"onUntil\": \"2025-01-10T23:07:55\"\n\
        }\n\
    },\n\
    \"week\": {\n\
        \"monday\": {\n\
            \"motors\": {\n\
                \"on\": [\n\
                    \"07:00:00\",\n\
                    \"20:00:00\"\n\
                ],\n\
                \"off\": [\n\
                    \"16:00:00\"\n\
                ]\n\
            },\n\
            \"lights\": {\n\
                \"on\": [\n\
                    \"07:00:00\"\n\
                ]\n\
            }\n\
        },\n\
        \"tuesday\": {\n\
            \"motors\": {\n\
                \"on\": [\n\
                    \"07:00:00\",\n\
                    \"20:00:00\"\n\
                ],\n\
                \"off\": [\n\
                    \"16:00:00\"\n\
                ]\n\
            }\n\
        },\n\
        \"wednesday\": {\n\
            \"motors\": {\n\
                \"on\": [\n\
                    \"07:00:00\",\n\
                    \"20:00:00\"\n\
                ],\n\
                \"off\": [\n\
                    \"10:00:00\"\n\
                ]\n\
            },\n\
            \"lights\": {\n\
                \"on\": [\n\
                    \"07:00:00\"\n\
                ]\n\
            }\n\
        },\n\
        \"thursday\": {\n\
            \"motors\": {\n\
                \"on\": [\n\
                    \"07:00:00\",\n\
                    \"20:00:00\"\n\
                ],\n\
                \"off\": [\n\
                    \"10:00:00\"\n\
                ]\n\
            },\n\
            \"lights\": {\n\
                \"on\": [\n\
                    \"07:00:00\"\n\
                ]\n\
            }\n\
        },\n\
        \"friday\": {\n\
            \"motors\": {\n\
                \"on\": [\n\
                    \"07:00:00\",\n\
                    \"20:00:00\"\n\
                ],\n\
                \"off\": [\n\
                    \"16:00:00\"\n\
                ]\n\
            },\n\
            \"lights\": {\n\
                \"on\": [\n\
                    \"07:00:00\"\n\
                ]\n\
            }\n\
        },\n\
        \"sunday\": {\n\
            \"lights\": {\n\
                \"on\": [\n\
                    \"07:00:00\"\n\
                ]\n\
            }\n\
        }\n\
    }\n\
}\n"
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Open a configuration file.  If the file does not exist it will
 * be created and filled with the given default contents. If
 * wCfgInit() has already been called this function will do nothing
 * and return success.
 *
 * @param filePath         the path and file name; cannot be empty.
 * @param defaultContents  the contents for the file if the file
 *                         needs to be created; may be empty, not
 *                         used if filePath already exists.  An error
 *                         will be returned if this is not valid JSON,
 *                         in which case you should delete the file
 *                         outside of this program and then supply
 *                         valid JSON.
 * @return                 zero on success, else negative error code.
 */
int wCfgInit(std::string filePath,
             std::string defaultContents = W_CFG_FILE_DEFAULT);

/** Refresh our understanding of the configuration file, in case
 * something (e.g. a web interface or other controlling entity)
 * has changed its contents.
 *
 * @return zero on success, else negative error code.
 */
int wCfgRefresh();

/** Get whether the configuration file says that the motors
 * should currently be on or off.  If there is no configuration
 * file entry of this nature, or wCfgOpen()/wCfgCreate() have
 * not been called, this will return true.  Call wCfgRefresh()
 * first to get an up-to-date verdict.
 *
 * @return true if the motors should be on, else false.
 */
bool wCfgMotorsOn();

/** Get whether the configuration file says that the lights
 * should currently be on or off.  If there is no configuration
 * file entry of this nature, or wCfgOpen()/wCfgCreate() have
 * not been called, this will return true.  Call wCfgRefresh()
 * first to get an up-to-date verdict.
 *
 * @return true if the lights should be on, else false.
 */
bool wCfgLightsOn();

/** Close the configuration file and free resources.
 */
void wCfgDeinit();

#endif // _W_CFG_H_

// End of file
