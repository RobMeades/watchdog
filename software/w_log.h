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

// The CPP stuff.
#include <chrono>
#include <string>
#include <iomanip>
#include <iostream>

// The Linux/Posix stuff.
#include <sys/time.h>

// Other parts of watchdog.
#include <w_util.h>

/** @file
 * @brief The logging API for the watchdog application; this stuff has to
 * be in header file because of the template stuff: what a mess!
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#define W_LOG_TAG "Watchdog"

// ANSI colour codes for printing.
#define W_ANSI_COLOUR_RESET "\u001b[0m"
#define W_ANSI_COLOUR_BRIGHT_WHITE "\u001b[37;1m"
#define W_ANSI_COLOUR_BRIGHT_GREEN "\u001b[32;1m"
#define W_ANSI_COLOUR_BRIGHT_YELLOW "\u001b[33;1m"
#define W_ANSI_COLOUR_BRIGHT_RED "\u001b[31;1m"
#define W_ANSI_COLOUR_BRIGHT_MAGENTA "\u001b[35;1m"

// Prefixes for info, warning and error strings.
#define W_INFO W_ANSI_COLOUR_BRIGHT_GREEN "INFO  " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET
#define W_WARN W_ANSI_COLOUR_BRIGHT_YELLOW "WARN  " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET
#define W_ERROR W_ANSI_COLOUR_BRIGHT_RED "ERROR " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET
#define W_DEBUG W_ANSI_COLOUR_BRIGHT_MAGENTA "DEBUG " W_ANSI_COLOUR_BRIGHT_WHITE W_LOG_TAG W_ANSI_COLOUR_RESET

// Logging macros: one-call.
#define W_LOG_INFO(...) wLog(W_LOG_TYPE_INFO, __LINE__, __VA_ARGS__)
#define W_LOG_WARN(...) wLog(W_LOG_TYPE_WARN, __LINE__, __VA_ARGS__)
#define W_LOG_ERROR(...) wLog(W_LOG_TYPE_ERROR, __LINE__, __VA_ARGS__)
#define W_LOG_DEBUG(...) wLog(W_LOG_TYPE_DEBUG, __LINE__, __VA_ARGS__)

// Logging macros: multiple calls.
#define W_LOG_INFO_START(...) wLogStart(W_LOG_TYPE_INFO, __LINE__, __VA_ARGS__)
#define W_LOG_WARN_START(...) wLogStart(W_LOG_TYPE_WARN, __LINE__, __VA_ARGS__)
#define W_LOG_ERROR_START(...) wLogStart(W_LOG_TYPE_ERROR, __LINE__, __VA_ARGS__)
#define W_LOG_DEBUG_START(...) wLogStart(W_LOG_TYPE_DEBUG, __LINE__, __VA_ARGS__)
#define W_LOG_INFO_MORE(...) wLogMore(W_LOG_TYPE_INFO, __VA_ARGS__)
#define W_LOG_WARN_MORE(...) wLogMore(W_LOG_TYPE_WARN, __VA_ARGS__)
#define W_LOG_ERROR_MORE(...) wLogMore(W_LOG_TYPE_ERROR, __VA_ARGS__)
#define W_LOG_DEBUG_MORE(...) wLogMore(W_LOG_TYPE_DEBUG, __VA_ARGS__)
#define W_LOG_INFO_END wLogEnd(W_LOG_TYPE_INFO)
#define W_LOG_WARN_END wLogEnd(W_LOG_TYPE_WARN)
#define W_LOG_ERROR_END wLogEnd(W_LOG_TYPE_ERROR)
#define W_LOG_DEBUG_END wLogEnd(W_LOG_TYPE_DEBUG)

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** The types of log print.  Values are important as they are
 * used as indexes into arrays.
 */
typedef enum {
    W_LOG_TYPE_INFO = 0,
    W_LOG_TYPE_WARN = 1,
    W_LOG_TYPE_ERROR = 2,
    W_LOG_TYPE_DEBUG = 3
} wLogType_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Array of log prefixes for the different log types, must be in
// the same order as wLogType_t.
static const char *gLogPrefixStr[] = {W_INFO, W_WARN, W_ERROR, W_DEBUG};

// Array of log destinations for the different log types, must be in
// the same order as wLogType_t.
static FILE *gLogDestination[] = {stdout, stdout, stderr, stdout};

/* ----------------------------------------------------------------
 * FUNCTION IMPLEMENTATIONS
 * -------------------------------------------------------------- */

// Print the start of a logging message.
template<typename ... Args>
void wLogStart(wLogType_t type, unsigned int line, Args ... args)
{
    FILE *destination = gLogDestination[type];
    const char *prefix = gLogPrefixStr[type];
    char buffer[32];
    timeval now;

    gettimeofday(&now, NULL);
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", gmtime(&(now.tv_sec)));

    fprintf(destination, "%s.%06ldZ ", buffer, now.tv_usec);
    fprintf(destination, "%s[%4d]: ", prefix, line);
    fprintf(destination, args...);
}

// Print the middle of a logging message, after logStart()
// has been called and before logEnd() is called.
template<typename ... Args>
void wLogMore(wLogType_t type, Args ... args)
{
    FILE *destination = gLogDestination[type];

    fprintf(destination, args...);
}

// Print the end of a logging message, after logStart()
// or logMore() has been called.
template<typename ... Args>
void wLogEnd(wLogType_t type)
{
    FILE *destination = gLogDestination[type];

    fprintf(destination, "\n");
}

// Print a single-line logging message.
template<typename ... Args>
void wLog(wLogType_t type, unsigned int line, Args ... args)
{
    wLogStart(type, line, args...);
    wLogEnd(type);
}

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Print the start of a logging message; this is not intended
 * to be called directly, please use W_LOG_XXX_START() instead.
 */
template<typename ... Args>
void wLogStart(wLogType_t type, unsigned int line, Args ... args);

/** Print the middle of a logging message, after logStart()
 * has been called and before logEnd() is called; this is not intended
 * to be called directly, please use W_LOG_XXX_MORE() instead.
 */
template<typename ... Args>
void wLogMore(wLogType_t type, Args ... args);

/** Print the end of a logging message, after logStart()
 * or logMore() has been called; this is not intended
 * to be called directly, please use W_LOG_XXX_END() instead.
 */
template<typename ... Args>
void wLogEnd(wLogType_t type);

/** Print a single-line logging message; this is not intended
 * to be called directly, please use W_LOG_XXX() instead.
 */
template<typename ... Args>
void wLog(wLogType_t type, unsigned int line, Args ... args);

// End of file
