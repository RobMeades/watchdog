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
 * @brief Implementation of the configuration API for the watchdog
 * application.
 *
 * This code makes use of cJSON hence must be linked with libcjson.
 */

// The CPP stuff.
#include <cstring>
#include <string>
#include <mutex>
#include <vector>
#include <algorithm>  // For std::sort()

// The Linux/Posix stuff.
#include <unistd.h>
#include <fcntl.h>   // For the file flags O_RDWR etc.
#include <time.h>
#include <sys/stat.h> // For chmod()

// The cJSON stuff.
#include <cJSON.h>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>

// Us.
#include <w_cfg.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_CFG_FILE_EXTRA_SIZE_BYTES
/** Some extra memory to allocate when reading the configuration
 * file in case it is being written-to by someonen else while
 * we are trying to find out how big it is.
 */
# define W_CFG_FILE_EXTRA_SIZE_BYTES (1024 * 5)
#endif

#ifndef W_CFG_TIME_UNIX_MIN
/** The minimum value for the current Unix time, used to check
 * if our clock is vaguely valid. 1736553600 corresponds to
 * midnight on 11 January 2025.
 */
# define W_CFG_TIME_UNIX_MIN 1736553600
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Structure to contain all of the possible outcomes from parsing
 * the configuration file.
 */
typedef struct {
    bool motorsOff;
    bool lightsOff;
} wCfg_t;

/** A structure to hold a JSON key and where its "off" entry is
 * stored in wCfg_t.
 * Note: everything in here uses "offNotOn" rather than the more
 * conventional "onNotOff" since the motors are by default on,
 * creating a variable left at zero will give it the default state
 * of "on" automatically, "off" is the exceptional state.
 */
typedef struct {
    const char *key;
    bool *offNotOn;
} wCfgThingOff_t;

/** A structure to hold a JSON key that can appear on a day of the week
 * for the motors or for the lights, and what it means in terms of
 * the offNotOn state of that thing.
 */
typedef struct {
    const char *key;
    bool offNotOn;
} wCfgOffOnItem_t;

/** A structure to hold an on or off time.
 */
typedef struct {
    time_t time;
    bool offNotOn;
} wCfgSwitchTime_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Mutex to protect the API.
static std::mutex gMutex;

// The configuration file handle.
static int gCfgFd = -1;

// Storage for the outcome of parsing the configuration file.
static wCfg_t gCfg = {0};

// The JSON elements to switch things off: their JSON keys and where
// the offNotOn values are stored in wCfg_t.
static const wCfgThingOff_t gThingOff[] = {{.key = "motors",
                                            .offNotOn = &(gCfg.motorsOff)},
                                           {.key = "lights",
                                            .offNotOn = &(gCfg.lightsOff)}};

// The days of the week, as they would appear in the configuration
// file; in this array they must be in the order of the days of the
// week and must start with the first day of the week, which must
// be Monday.
static const char *gDaysOfWeek[] = {"monday", "tuesday", "wednesday",
                                    "thursday", "friday", "saturday",
                                    "sunday"};

// The types of time entry each day can have for a thing, i.e.
// for the motors or the lights.
static const wCfgOffOnItem_t gOffOnItem[] = {{.key = "on",
                                              .offNotOn = false},
                                             {.key = "off",
                                              .offNotOn = true}};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Open a file with the given flags and return the file handle or
// a negative error code on failure.
static int openFile(const char *filePath, int flags = O_RDONLY)
{
    int fdOrErrorCode = -EINVAL;

    fdOrErrorCode = open(filePath, flags);
    if (fdOrErrorCode < 0) {
        fdOrErrorCode = -errno;
    }

    return fdOrErrorCode;
}

// Get the Unix local time that represents the start of the
// current week, i.e. midnight of the previous Sunday.
static time_t startOfWeekDateTime(time_t timeNow)
{
    time_t dateTimeOrErrorCode = -EINVAL;

    if (timeNow > W_CFG_TIME_UNIX_MIN) {
        struct tm t = {};
        if (localtime_r(&timeNow, &t)) {
            // Now have the current local time in a tm struct,
            // work out how far we are from midnight on
            // Sunday; tm_wday is the zero-based number
            // of days since midnight on Saturday (i.e.
            // Sunday is day 0), so we need to shift it
            time_t time = ((t.tm_wday + 6) % 7) * (60 * 60 * 24);
            time += t.tm_hour * (60 * 60);
            time += t.tm_min * 60;
            time += t.tm_sec;
            // Subtract that from the time now to get the answer
            dateTimeOrErrorCode = timeNow - time;
        } else {
            dateTimeOrErrorCode = -errno;
        }
    }

    return dateTimeOrErrorCode;
}

// Parse a string in HH:MM:SS format and return the number of
// seconds since midnight.
static int parseTime(const char *timeStr)
{
    int timeOrErrorCode = -EINVAL;

    if (timeStr) {
        struct tm t = {};
        if (strptime(timeStr, "%T", &t)) {
            // Have a valid tm struct: convert it to a time in seconds
            timeOrErrorCode = t.tm_hour * 60 * 60;
            timeOrErrorCode += t.tm_min * 60;
            timeOrErrorCode += t.tm_sec;
        }
    }

    return timeOrErrorCode;
}

// Parse a cJSON object for a key whose value is a local date/time
// in ISO8601 format (but ignoring milliseconds and any marker)
// and return that local time as a Unix time.
static time_t parseJsonDateTime(const cJSON *objectJson, const char *dateTimeKey)
{
    time_t timeOrErrorCode = -EINVAL;

    if (objectJson && dateTimeKey) {
        const cJSON *dateTimeJson = cJSON_GetObjectItemCaseSensitive(objectJson,
                                                                     dateTimeKey);
        if (cJSON_IsString(dateTimeJson) && (dateTimeJson->valuestring != nullptr)) {
            // Parse the time
            struct tm t = {};
            if (strptime(dateTimeJson->valuestring, "%FT%T", &t)) {
                // Have a valid tm struct: convert it to (local) time_t
                timeOrErrorCode = mktime(&t);
            }
        }
    }

    return timeOrErrorCode;
}


// Sort switch times in ascending order, used only by the sort()
// function in parseJson().
static bool compareSwitchTime(wCfgSwitchTime_t switchTimeA,
                              wCfgSwitchTime_t switchTimeB) {
    return switchTimeA.time < switchTimeB.time;
}

// Parse a buffer of JSON into our configuration.
// NOTE: this function will fail if the real time clock
// has not yet been set to a valid time.
// IMPORTANT: gMutex should be locked before this is called.
static int parseJson(const char *buffer, unsigned int sizeBytes)
{
    int errorCode = -EINVAL;
    time_t timeNow = 0;

    if (buffer && (time(&timeNow) >= 0) && (timeNow > W_CFG_TIME_UNIX_MIN)) {
        errorCode = -EPROTO;
        // Get cJSON to parse the buffer
        cJSON *json = cJSON_Parse(buffer);
        if (json) {
            // If the JSON is parseable, we're good as far as errors are concerned
            errorCode = 0;
            // Get the first "week" item, if present, in case we need to use it
            const cJSON *weekJson = cJSON_GetObjectItemCaseSensitive(json, "week");
            // And work out the Unix time that corresponds to the start of this week
            time_t startOfWeek = startOfWeekDateTime(timeNow);
            if (startOfWeek >= 0) {
                // Check for override date/times for the motors and the lights
                for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gThingOff); x++) {
                    const wCfgThingOff_t *thing = &(gThingOff[x]);
                    // Check for the first "overrideOffUntil" override for this thing
                    // at the top level
                    const cJSON *thingJson = cJSON_GetObjectItemCaseSensitive(json, thing->key);
                    if (cJSON_IsObject(thingJson)) {
                        time_t time = parseJsonDateTime(thingJson, "overrideOffUntil");
                        if (time > timeNow) {
                            // There is an "overrideOffUntil" date/time and it is greater
                            // than the time now, so the thing must be off
                            *(thing->offNotOn) = true;
                        }
                    }
                    if (!*(thing->offNotOn) && weekJson) {
                        // If the thing is still on, make a list of the
                        // times of every off/on entry for the same thing
                        // during the week
                        std::vector<wCfgSwitchTime_t> switchTimeList;
                        for (unsigned int day = 0; day < W_UTIL_ARRAY_COUNT(gDaysOfWeek); day++) {
                            // Find the first occurrence of this day in the week
                            const cJSON *dayJson = cJSON_GetObjectItemCaseSensitive(weekJson,
                                                                                    gDaysOfWeek[day]);
                            if (cJSON_IsObject(dayJson)) {
                                time_t startOfDay = startOfWeek + (day * (24 * 60 * 60));
                                // Get the first motors/lights item (== thing) within this day
                                thingJson = cJSON_GetObjectItemCaseSensitive(dayJson, thing->key);
                                if (cJSON_IsObject(thingJson)) {
                                    // For each array type we understand ("off"/"on")...
                                    for (unsigned int arrayType = 0;
                                         arrayType < W_UTIL_ARRAY_COUNT(gOffOnItem);
                                         arrayType++) {
                                        const wCfgOffOnItem_t *offOnItem = &(gOffOnItem[arrayType]);
                                        // ...look for the first item of that type in the thing and
                                        // check that it is an array
                                        const cJSON *arrayJson = cJSON_GetObjectItemCaseSensitive(thingJson,
                                                                                                  offOnItem->key);
                                        if (cJSON_IsArray(arrayJson) && (cJSON_GetArraySize(arrayJson) > 0)) {
                                            int size = cJSON_GetArraySize(arrayJson);
                                            wCfgSwitchTime_t switchTime = {};
                                            switchTime.time = -1;
                                            // Iterate over the items in the array, which should
                                            // be strings representing times
                                            for (int index = 0; index < size; index++) {
                                                const cJSON *timeJson = cJSON_GetArrayItem(arrayJson, index);
                                                if (cJSON_IsString(timeJson) && (timeJson->valuestring != nullptr)) {
                                                    switchTime.offNotOn = offOnItem->offNotOn;
                                                    switchTime.time = parseTime(timeJson->valuestring);
                                                    if (switchTime.time >= 0) {
                                                        switchTime.time += startOfDay;
                                                        switchTimeList.push_back(switchTime);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        // We now have a list of off/on switch times: sort the list in ascending
                        // order of time
                        std::sort(switchTimeList.begin(), switchTimeList.end(), compareSwitchTime);
                        // Walk through the list up to the current time to determine the
                        // off state now
                        for (auto switchTime = switchTimeList.begin();
                             (switchTime != switchTimeList.end()) && (switchTime->time <= timeNow);
                             switchTime++) {
                            *(thing->offNotOn) = switchTime->offNotOn;
                        }
                    }
                }
            } else {
                errorCode = (int) startOfWeek;
            }

            // Free memory
            cJSON_Delete(json);

        } else {
            // Point out where the JSON parsing error was in the buffer if possible
            const char *errorStr = cJSON_GetErrorPtr();
            if (errorStr != nullptr) {
                W_LOG_ERROR("JSON error in cfg file before %s!", errorStr);
            }
        }
    }

    return errorCode;
}

// Parse the given configuration file into our configuration.
// IMPORTANT: gMutex should be locked before this is called.
static int parseFile(int fd)
{
    int errorCode = -EBADF;

    if (fd >= 0) {
        // Move to the end of the file to measure its size
        errorCode = lseek(fd, 0, SEEK_END);
        if (errorCode >= 0) {
            // ALlocate that many bytes, plus some
            // slack in case something has gone and
            // written to the file while we were thinking
            int sizeBytes = errorCode + W_CFG_FILE_EXTRA_SIZE_BYTES;
            errorCode = -ENOMEM;
            char *buffer = (char *) malloc(sizeBytes);
            if (buffer) {
                // Move to the start of the file
                errorCode = lseek(fd, 0, SEEK_SET);
                if (errorCode == 0) {
                    // Read the whole file
                    int totalRead = 0;
                    do {
                        errorCode = read(fd, buffer + totalRead, sizeBytes);
                        if (errorCode > 0) {
                            totalRead += errorCode;
                            sizeBytes -= errorCode;
                        }
                    } while ((errorCode > 0) && (sizeBytes >= 0));
                    if (errorCode == 0) {
                        // Parse the JSON that should be in the buffer
                        errorCode = parseJson(buffer, totalRead);
                    } else if (errorCode > 0) {
                        // Still stuff to read, not enough buffer space
                        errorCode = -ENOBUFS;
                    } else {
                        errorCode = -errno;
                    }
                } else if (errorCode > 0) {
                    // Couldn't seek to the start of the file:
                    // flag some sort of error
                    errorCode = -1;
                } else {
                    errorCode = -errno;
                }

                // Free memory
                free(buffer);

            }
        } else {
            errorCode = -errno;
        }
    }

    return errorCode;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Open a configuration file.
int wCfgInit(std::string filePath, std::string defaultContents)
{
    int errorCode = 0;

    if (gCfgFd < 0) {
        errorCode = -EINVAL;
        if (!filePath.empty()) {

            gMutex.lock();

            const char *filePathCStr = filePath.c_str();
            int fd = -1;
            errorCode = openFile(filePathCStr);
            if (errorCode >= 0) {
                fd = errorCode;
                errorCode = 0;
            } else {
                // Can't open the file, try to create it
                errorCode = 0;
                std::string directories = wUtilDirectoryPathGet(filePath);
                if (!directories.empty()) {
                    // Make sure the directories exist
                    system(std::string("mkdir -p " + directories).c_str());
                }
                // If we have default contents to write, create the
                // file read/write, else create it read-only
                int flags = O_RDWR;
                if (defaultContents.empty()) {
                    flags = O_RDONLY;
                }
                flags |= O_CREAT;
                errorCode = openFile(filePathCStr, flags);
                if (errorCode >= 0) {
                    fd = errorCode;
                    errorCode = 0;
                    // Have a file now: if we have default contents,
                    // write them to it
                    if (!defaultContents.empty()) {
                        errorCode = write(fd, defaultContents.c_str(),
                                          defaultContents.size());
                        if (errorCode == (int) defaultContents.size()) {
                            errorCode = 0;
                        } else if (errorCode < 0) {
                            errorCode = -errno;
                        } else {
                            // Couldn't write all of the default contents:
                            // flag some sort of error
                            errorCode = -1;
                        }
                        // Whatever the outcome we close the file
                        close(fd);
                        fd = -1;
                        if (errorCode == 0) {
                            // Set 0660 permissions so that anyone
                            // can read or write the file
                            errorCode = chmod(filePathCStr, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
                            if (errorCode == 0) {
                                // We were successfully able to write the
                                // default contents to the file: now open
                                // it as read-only since that is all we need
                                errorCode = openFile(filePathCStr);
                                if (errorCode >= 0) {
                                    fd = errorCode;
                                    errorCode = 0;
                                }
                            } else {
                                errorCode = -errno;
                            }
                        }
                    }
                }
            }

            if (fd >= 0) {
                // Parse the file into our configuration
                errorCode = parseFile(fd);
                if (errorCode == 0) {
                    // Remember the file handle
                    gCfgFd = fd;
                } else {
                    // Clean up on error
                    close(fd);
                    fd = -1;
                }
            }

            gMutex.unlock();
        }
    }

    return errorCode;
}

// Refresh our understanding of the contents of the
// configuration file.
int wCfgRefresh()
{
    int errorCode = -EBADF;

    gMutex.lock();

    if (gCfgFd >= 0) {
        errorCode = parseFile(gCfgFd);
    }

    gMutex.unlock();

    return errorCode;
}

// Get whether the motors should be on or off.
bool wCfgMotorsOn()
{
    bool isOn;

    gMutex.lock();

    isOn = !gCfg.motorsOff;

    gMutex.unlock();

    return isOn;
}

// Get whether the lights should be on or off.
bool wCfgLightsOn()
{
    bool isOn;

    gMutex.lock();

    isOn = !gCfg.lightsOff;

    gMutex.unlock();

    return isOn;
}

// Close the configuration file and free resources.
void wCfgDeinit()
{
    gMutex.lock();

    if (gCfgFd >= 0) {
        close(gCfgFd);
        gCfgFd = -1;
        // Set gCfg back to defaults, in case one of
        // the wCfgXxxOn() APIs get called while we
        // are deinitialised
        memset(&gCfg, 0, sizeof(gCfg));
    }

    gMutex.unlock();
}

// End of file
