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
 * @brief The implementation of the LED control API for the watchdog
 * application.
 */

// The CPP stuff.
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>

// The Linux/Posix stuff.
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>
#include <w_msg.h>
#include <w_gpio.h>

// Us.
#include <w_led.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES: MISC
 * -------------------------------------------------------------- */

/** Identify the LED modes.
 */
typedef enum {
    W_LED_MODE_TYPE_CONSTANT,
    W_LED_MODE_TYPE_BREATHE
} wLedModeType_t;

/** LED level control.
 */
typedef struct {
    unsigned int targetPercent;
    int changePercent;        // The amount to change by
    uint64_t changeInterval;  // The interval between ticks to make a change
    uint64_t changeStartTick; // The tick at which to begin a level change
} wLedLevel_t;

/** Control state for constant mode.
 */
typedef struct {
    wLedLevel_t level;
} wLedModeConstant_t;

/** Control state for breathe mode.
 */
typedef struct {
    wLedLevel_t levelAverage;
    unsigned int levelAmplitudePercent;
    unsigned int rateMilliHertz;
    uint64_t offsetLeftToRightTicks;
} wLedModeBreathe_t;

/** Union of LED modes.
 */
typedef union {
    wLedModeConstant_t constant;
    wLedModeBreathe_t breathe;
} wLedMode_t;

/** Morse overlay.
 */
typedef struct {
    char sequenceStr[W_LED_MORSE_MAX_SIZE]; // Null terminated string
    unsigned int repeat;
    unsigned int levelPercent;
    unsigned int durationDotTicks;
    unsigned int durationDashTicks;
    unsigned int durationGapLetterTicks;
    unsigned int durationGapWordTicks;
    unsigned int durationGapRepeatTicks;
} wLedOverlayMorse_t;

/** Wink overlay.
 */
typedef struct {
    unsigned int durationTicks;
} wLedOverlayWink_t;

/** Random blink overlay.
 */
typedef struct {
    uint64_t intervalTicks;
    uint64_t rangeTicks;
    uint64_t durationTicks;
    uint64_t lastBlinkTicks;
} wLedOverlayRandomBlink_t;

/** Control state for one or more LEDs.
 */
typedef struct {
    wLedModeType_t modeType;
    wLedMode_t mode;
    unsigned int levelAveragePercent;
    unsigned int levelAmplitudePercent;
    uint64_t lastChangeTick;
    wLedOverlayMorse_t *morse;
    wLedOverlayWink_t *wink;
} wLedState_t;

/** Context that must be maintained for the LED message handlers.
 */
typedef struct {
    int fd;
    std::thread thread;
    std::mutex mutex;
    uint64_t nowTick;
    wLedOverlayRandomBlink_t *randomBlink;
    wLedState_t ledState[W_LED_MAX_NUM];
} wLedContext_t;

/** LED control sub-structure used by the messages.
 */
typedef struct {
    wLed_t led;
    int offsetLeftToRightMs; // Used if led is W_LED_BOTH
} wLedApply_t;

/* ----------------------------------------------------------------
 * TYPES: MESSAGES
 * -------------------------------------------------------------- */

/** LED message types.
 */
typedef enum {
    W_LED_MSG_TYPE_MODE_CONSTANT,         // wLedMsgBodyModeConstant_t
    W_LED_MSG_TYPE_MODE_BREATHE,          // wLedMsgBodyModeBreathe_t
    W_LED_MSG_TYPE_OVERLAY_MORSE,         // wLedMsgBodyOverlayMorse_t
    W_LED_MSG_TYPE_OVERLAY_WINK,          // wLedMsgBodyOverlayWink_t
    W_LED_MSG_TYPE_OVERLAY_RANDOM_BLINK,  // wLedMsgBodyOverlayRandomBlink_t
    W_LED_MSG_TYPE_LEVEL_SCALE            // wLedMsgBodyLevelScale_t
} wLedMsgType_t;

/** The message body structure corresponding to W_LED_MSG_TYPE_MODE_CONSTANT.
 */
typedef struct {
    wLedApply_t apply;
    unsigned int levelPercent;
    unsigned int rampMs;       // The time to get to levelPercent in milliseconds
} wLedMsgBodyModeConstant_t;

/** The message body structure corresponding to W_LED_MSG_TYPE_MODE_BREATHE.
 */
typedef struct {
    wLedApply_t apply;
    unsigned int rateMilliHertz;
    unsigned int levelAveragePercent;
    unsigned int levelAmplitudePercent;
    unsigned int rampMs;       // The time to get to averageLevelPercent in milliseconds
} wLedMsgBodyModeBreathe_t;

/** The message body structure corresponding to W_LED_MSG_TYPE_OVERLAY_MORSE.
 */
typedef struct {
    wLedApply_t apply;
    wLedOverlayMorse_t overlay;
} wLedMsgBodyOverlayMorse_t;

/** The message body structure corresponding to W_LED_MSG_TYPE_OVERLAY_WINK.
 */
typedef struct {
    wLedApply_t apply;
    wLedOverlayWink_t overlay;
} wLedMsgBodyOverlayWink_t;

/** The message body structure corresponding to W_LED_MSG_TYPE_OVERLAY_RANDOM_BLINK.
 */
typedef struct {
    wLedOverlayRandomBlink_t overlay;
} wLedMsgBodyOverlayRandomBlink_t;

/** The message body structure corresponding to W_LED_MSG_TYPE_LEVEL_SCALE.
 */
typedef struct {
    wLedApply_t apply;
    unsigned int percent; // The percentage value to scale all levels by
    unsigned int rampMs;  // The time to get to the new scale in milliseconds
} wLedMsgBodyLevelScale_t;

/** Union of LED message bodies; if you add a member here you must add a type for it in
 * wLedMsgType_t.
 */
typedef union {
    wLedMsgBodyModeConstant_t modeConstant;             // W_LED_MSG_TYPE_MODE_CONSTANT
    wLedMsgBodyModeBreathe_t modeBreathe;               // W_LED_MSG_TYPE_MODE_BREATHE
    wLedMsgBodyOverlayMorse_t overlayMorse;             // W_LED_MSG_TYPE_OVERLAY_MORSE
    wLedMsgBodyOverlayWink_t overlayWink;               // W_LED_MSG_TYPE_OVERLAY_WINK
    wLedMsgBodyOverlayRandomBlink_t overlayRandomBlink; // W_LED_MSG_TYPE_OVERLAY_RANDOM_BLINK
    wLedMsgBodyLevelScale_t levelScale;                 // W_LED_MSG_TYPE_LEVEL_SCALE
} wLedMsgBody_t;

/** A structure containing the message handling function and the message
 * type it handles, for use in gMsgHandler[].
 */
typedef struct {
    wLedMsgType_t msgType;
    wMsgHandlerFunction_t *function;
} wLedMsgHandler_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// NOTE: there are more messaging-related variables below
// the definition of the message handling functions.

// A local keep-going flag.
static bool gKeepGoing = false;

// A place to store our message queue ID.
static int gMsgQueueId = -1;

// Timer that is employed by ledLoop().
static int gTimerFd = -1;

// LED context.
static wLedContext_t gContext = {};

// Table of wLed_t to LED pin.
static const unsigned int gLedToPin[] = {W_GPIO_PIN_OUTPUT_EYE_LEFT,
                                         W_GPIO_PIN_OUTPUT_EYE_RIGHT};

// A table of sine-wave magnitudess for a quarter wave, scaled by 100;
// with a W_LED_TICK_TIMER_PERIOD_MS of 20, these 50 entries would
// take 1 second, so the rate for a full wave would be 4 Hertz.
static const int gSinePercent[] = {  0,  3,  6, 9,  13, 16,  19,  22,  25,  28,
                                    31, 34, 37, 40, 43, 45,  48,  51,  54,  56,
                                    59, 61, 64, 66, 68, 71,  73,  75,  77,  79,
                                    81, 83, 84, 86, 88, 89,  90,  92,  93,  94,
                                    95, 96, 97, 98, 99, 99, 100, 100, 100, 100};

// Names for each of the LEDs, for debug prints only; order must
// match wLed_t.
static const char *gLedStr[] = {"left", "right", "both"};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Take an integer LED level as a percentage and return an in-range
// unsigned int that can be applied to a PWM pin.
static unsigned int limitLevel(int levelPercent)
{
    if (levelPercent > 100) {
        levelPercent = 100;
    } else if (levelPercent < 0) {
        levelPercent = 0;
    }

    return (unsigned int) levelPercent;
}

// Set a random blink, if required
static int randomBlink(wLedOverlayRandomBlink_t *randomBlink,
                       uint64_t nowTick)
{
    int levelPercent = -1;

    if (randomBlink) {
        if ((randomBlink->lastBlinkTicks > 0) &&
            (randomBlink->lastBlinkTicks < nowTick) && 
            (nowTick - randomBlink->lastBlinkTicks < randomBlink->durationTicks)) {
            levelPercent = 0;
        } else {
            if (nowTick > randomBlink->lastBlinkTicks +
                          randomBlink->intervalTicks +
                          (randomBlink->rangeTicks * rand() / RAND_MAX) -
                          (randomBlink->rangeTicks / 2)) {
                randomBlink->lastBlinkTicks = nowTick;
                levelPercent = 0;
            }
        }
    }

    return levelPercent;
}

// Move any level changes on.
// IMPORTANT: the LED context must be locked before this is called.
static int updateLevel(wLed_t led, wLedState_t *state,
                       uint64_t nowTick, wLedLevel_t *levelAverage,
                       unsigned int levelAmplitudePercent = 0,
                       unsigned int rateMilliHertz = 0,
                       uint64_t offsetLeftToRightTicks = 0)
{
    int levelPercentOrErrorCode = -EINVAL;

    if (state && levelAverage) {
        int newLevelPercent = (int) state->levelAveragePercent;
        if ((state->levelAveragePercent != levelAverage->targetPercent) &&
            (nowTick > levelAverage->changeStartTick) &&
            (nowTick - state->lastChangeTick > levelAverage->changeInterval)) {
            // We are ramping the average level
            newLevelPercent += levelAverage->changePercent; 
            state->levelAveragePercent = limitLevel(newLevelPercent);
            state->lastChangeTick = nowTick;
            if (state->levelAveragePercent == levelAverage->targetPercent) {
                // Done with any ramp
                levelAverage->changeInterval = 0;
                levelAverage->changePercent = 0;
            }
        }

        if (levelAmplitudePercent == 0) {
            // Constant level otherwise, just set it
            levelPercentOrErrorCode = state->levelAveragePercent;
        } else {
            // Doing a "breathe" around the average
            unsigned int index = (unsigned int) nowTick;
            // Add the sine-wave left/right offset if there is one
            if ((offsetLeftToRightTicks > 0) && (led == W_LED_RIGHT)) {
                index += offsetLeftToRightTicks;
            } else if ((offsetLeftToRightTicks < 0) && (led == W_LED_LEFT)) {
                index += -offsetLeftToRightTicks;
                if (index < 0) {
                    // Prevent underrun by wrapping about the length of a sine wave
                    index += W_UTIL_ARRAY_COUNT(gSinePercent) * 4;
                }
            }
            // The sine wave table (which is a quarter of a sine wave), with a
            // W_LED_TICK_TIMER_PERIOD_MS of 20 ms, is 4 Hertz
            int rateHertz = (1000 / W_LED_TICK_TIMER_PERIOD_MS) * 4 / W_UTIL_ARRAY_COUNT(gSinePercent);
            // Scale by rateMilliHertz
            index *= (rateHertz * 1000) / rateMilliHertz;
            // Index is across a full wave, so four times the sine table
            index = index % (W_UTIL_ARRAY_COUNT(gSinePercent) * 4);
            // This is W_LOG_DEBUG_MORE as this function is only called
            // from ledLoop(), which will already have started a debug print
            // Now map index into the sine table quarter-wave
            int multiplier = 1;
            if (index >= W_UTIL_ARRAY_COUNT(gSinePercent) * 2) {
                // We're in the negative half of the sine wave
                multiplier = -1;
                if (index >= W_UTIL_ARRAY_COUNT(gSinePercent) * 3) {
                    // We're in the last quarter
                    index = (W_UTIL_ARRAY_COUNT(gSinePercent) - 1) - (index % W_UTIL_ARRAY_COUNT(gSinePercent));
                } else {
                    // We're in the third quarter
                    index = index % W_UTIL_ARRAY_COUNT(gSinePercent);
                }
            } else {
                // We're in the positive half of the sine wave
                if (index >= W_UTIL_ARRAY_COUNT(gSinePercent)) {
                    // We're in the second quarter
                    index = (W_UTIL_ARRAY_COUNT(gSinePercent) - 1) - (index % W_UTIL_ARRAY_COUNT(gSinePercent));
                }
            }
            newLevelPercent += ((int) levelAmplitudePercent) * gSinePercent[index] * multiplier / 100;
            levelPercentOrErrorCode = limitLevel(newLevelPercent);
        }
    }

    return levelPercentOrErrorCode;
}

// A loop to drive the dynamic behaviours of the LEDs.
static void ledLoop()
{
    if (gTimerFd >= 0) {
        uint64_t numExpiries;
        struct pollfd pollFd[1] = {};
        struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
        sigset_t sigMask;

        pollFd[0].fd = gTimerFd;
        pollFd[0].events = POLLIN | POLLERR | POLLHUP;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);

        W_LOG_DEBUG("LED loop has started.");

        while (gKeepGoing && wUtilKeepGoing()) {
            // Block waiting for our tick timer to go off or for
            // CTRL-C to land
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(gTimerFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries)) &&
                gContext.mutex.try_lock()) {

                // Set the level for a random blink, if there is one
                int initialLevelPercent = randomBlink(gContext.randomBlink,
                                                      gContext.nowTick);
                // Update the LED pins
                for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gContext.ledState); x++) {
                    wLedState_t *state = &(gContext.ledState[x]);
                    int levelPercent = initialLevelPercent;
                    if (state->morse) {
                        // If we are running a morse sequence, that
                        // takes priority, including over the blink

                        // TODO

                    }
                    if (levelPercent < 0) {
                        // Do the modes etc. if the level hasn't been
                        // set by a blink or by morse
                        switch (state->modeType) {
                            case W_LED_MODE_TYPE_CONSTANT:
                            {
                                wLedModeConstant_t *mode = &(state->mode.constant);
                                // Progress any change of level
                                levelPercent = updateLevel((wLed_t) x, state,
                                                            gContext.nowTick,
                                                            &(mode->level));
                            }
                            break;
                            case W_LED_MODE_TYPE_BREATHE:
                            {
                                wLedModeBreathe_t *mode = &(state->mode.breathe);
                                // Progress any change of level
                                levelPercent = updateLevel((wLed_t) x, state,
                                                            gContext.nowTick,
                                                            &(mode->levelAverage),
                                                            mode->levelAmplitudePercent,
                                                            mode->rateMilliHertz,
                                                            mode->offsetLeftToRightTicks);
                            }
                            break;
                            default:
                                break;
                        }
                        // Wink overlays the mode
                        if (state->wink) {

                            // TODO

                        }
                    }
                    // Apply the new level
                    if (levelPercent >= 0) {
                        wGpioPwmSet(gLedToPin[x], levelPercent);
                    }
                }

                gContext.nowTick++;
                gContext.mutex.unlock();
            }
        }
    }

    W_LOG_DEBUG("LED loop has exited.");
}

// Convert a time in milliseconds to the number of ticks of the
// LED loop.
static int64_t msToTicks(int64_t milliseconds)
{
    return milliseconds / W_LED_TICK_TIMER_PERIOD_MS;
}

// Convert a time in ticks of the LED loop into milliseconds.
static int64_t ticksToMs(int64_t ticks)
{
    return ticks * W_LED_TICK_TIMER_PERIOD_MS;
}

// Return the start tick for an LED change.
static uint64_t levelChangeStartSet(uint64_t nowTick, wLedApply_t *apply,
                                    wLed_t led)
{
    uint64_t changeStartTick = nowTick;

    if ((apply->led == W_LED_BOTH) &&
        (apply->offsetLeftToRightMs != 0)) {
        // Apply an offset if the incoming message was to set
        // both LEDs.  We don't handle the wrap here as the tick
        // is assumed to be an unsigned int64_t...?
        int64_t offsetTicks = msToTicks(apply->offsetLeftToRightMs);
        if ((offsetTicks > 0) && (led == W_LED_RIGHT)) {
            changeStartTick += offsetTicks;
        } else if ((offsetTicks < 0) && (led == W_LED_LEFT)) {
            changeStartTick += -offsetTicks;
        }
    }

    return changeStartTick;
}

// Return the tick-interval at which a ramped LED level should change;
// the amount to increment at each interval is returned in the last
// parameter.
static uint64_t levelChangeIntervalSet(unsigned int rampMs,
                                       unsigned int targetLevelPercent,
                                       unsigned int nowLevelPercent,
                                       int *changePercent)
{
    int64_t changeInterval = INT64_MAX;
    int64_t changePeriod = msToTicks(rampMs);
    int levelChangePercent = targetLevelPercent - nowLevelPercent;

    if (levelChangePercent != 0) {
        changeInterval = changePeriod / levelChangePercent;
        // Make sure we return a positive interval
        if (changeInterval < 0) {
            changeInterval = -changeInterval;
        }
        if (changePercent) {
            // Set the change per interval
            *changePercent = levelChangePercent;
            if (changeInterval > 0) {
                *changePercent = levelChangePercent * changeInterval / changePeriod;
                if (*changePercent == 0) {
                    // Avoid rounding errors leaving us in limbo
                    *changePercent = 1;
                }
            }
        }
    }

    return (uint64_t) changeInterval;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wLedMsgBodyModeConstant_t
 * -------------------------------------------------------------- */

// Update function called only by msgHandlerLedModeConstant().
// IMPORTANT: the LED context must be locked before this is called.
static void msgHandlerLedModeConstantUpdate(wLed_t led,
                                            wLedState_t *state,
                                            uint64_t nowTick,
                                            wLedMsgBodyModeConstant_t *modeSrc)
{
    state->modeType = W_LED_MODE_TYPE_CONSTANT;
    wLedModeConstant_t *modeDst = &(state->mode.constant);
    modeDst->level.targetPercent = modeSrc->levelPercent;
    modeDst->level.changeStartTick = levelChangeStartSet(nowTick,
                                                         &(modeSrc->apply),
                                                         led);
    modeDst->level.changeInterval = levelChangeIntervalSet(modeSrc->rampMs,
                                                           modeSrc->levelPercent,
                                                           state->levelAveragePercent,
                                                           &(modeDst->level.changePercent));
    // This is W_LOG_DEBUG_MORE since it will be within a sequence
    // of log prints in msgHandlerLedModeConstant()
    W_LOG_DEBUG_MORE(" (so start tick %06lld, interval %lld tick(s),"
                     " change per tick %d%%)",
                     modeDst->level.changeStartTick,
                     modeDst->level.changeInterval,
                     modeDst->level.changePercent);
}

// Message handler for wLedMsgBodyModeConstant_t.
static void msgHandlerLedModeConstant(void *msgBody, unsigned int bodySize,
                                      void *context)
{
    wLedMsgBodyModeConstant_t *msg = &(((wLedMsgBody_t *) msgBody)->modeConstant);
    wLedContext_t *ledContext = (wLedContext_t *) context;
    assert(bodySize == sizeof(*msg));

    W_LOG_DEBUG_START("HANDLER [%06lld]: wLedMsgBodyModeConstant_t (LED %d,"
                      " %d%%, ramp %d ms, offset %d ms)", ledContext->nowTick,
                      msg->apply.led, msg->levelPercent, msg->rampMs,
                      msg->apply.offsetLeftToRightMs);

    // Lock the LED context
    ledContext->mutex.lock();

    if (msg->apply.led < W_UTIL_ARRAY_COUNT(ledContext->ledState)) {
        // We're updating one LED
        wLedState_t *state = &(ledContext->ledState[msg->apply.led]);
        W_LOG_DEBUG_MORE("; %s LED mode %d, level %d%%, last change %06lld",
                         gLedStr[msg->apply.led], state->modeType,
                         state->levelAveragePercent, state->lastChangeTick);
        msgHandlerLedModeConstantUpdate(msg->apply.led, state, ledContext->nowTick, msg);
    } else {
        // Update both LEDs
        for (size_t x = 0; x < W_UTIL_ARRAY_COUNT(ledContext->ledState); x++) {
            wLedState_t *state = &(ledContext->ledState[x]);
            W_LOG_DEBUG_MORE("; %s LED mode %d, level %d%%, last change %06lld",
                             gLedStr[x], state->modeType,
                             state->levelAveragePercent, state->lastChangeTick);
            msgHandlerLedModeConstantUpdate((wLed_t) x, state, ledContext->nowTick, msg);
        }
    }
    W_LOG_DEBUG_MORE(".");
    W_LOG_DEBUG_END;

    // Unlock the context again
    ledContext->mutex.unlock();
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wLedMsgBodyModeBreathe_t
 * -------------------------------------------------------------- */

// Update function called only by wLedMsgBodyModeBreathe_t().
// IMPORTANT: the LED context must be locked before this is called.
static void msgHandlerLedModeBreatheUpdate(wLed_t led,
                                           wLedState_t *state,
                                           uint64_t nowTick,
                                           wLedMsgBodyModeBreathe_t *modeSrc)
{
    state->modeType = W_LED_MODE_TYPE_BREATHE;
    wLedModeBreathe_t *modeDst = &(state->mode.breathe);
    modeDst->rateMilliHertz = modeSrc->rateMilliHertz;
    modeDst->offsetLeftToRightTicks = msToTicks(modeSrc->apply.offsetLeftToRightMs);
    modeDst->levelAmplitudePercent = modeSrc->levelAmplitudePercent;
    modeDst->levelAverage.targetPercent = modeSrc->levelAveragePercent;
    modeDst->levelAverage.changeStartTick = levelChangeStartSet(nowTick,
                                                                &(modeSrc->apply),
                                                                led);
    modeDst->levelAverage.changeInterval = levelChangeIntervalSet(modeSrc->rampMs,
                                                                  modeSrc->levelAveragePercent,
                                                                  state->levelAveragePercent,
                                                                  &(modeDst->levelAverage.changePercent));
    // This is W_LOG_DEBUG_MORE since it will be within a sequence
    // of log prints in msgHandlerLedModeBreathe()
    W_LOG_DEBUG_MORE(" (so start tick %06lld, interval %lld tick(s),"
                     " change per tick %d%%)",
                     modeDst->levelAverage.changeStartTick,
                     modeDst->levelAverage.changeInterval,
                     modeDst->levelAverage.changePercent);
}

// Message handler for wLedMsgBodyModeBreathe_t.
static void msgHandlerLedModeBreathe(void *msgBody, unsigned int bodySize,
                                     void *context)
{
    wLedMsgBodyModeBreathe_t *msg = &(((wLedMsgBody_t *) msgBody)->modeBreathe);
    wLedContext_t *ledContext = (wLedContext_t *) context;
    assert(bodySize == sizeof(*msg));

    W_LOG_DEBUG_START("HANDLER [%06lld]: wLedMsgBodyModeBreathe_t (LED %d,"
                      " %d%% +/-%d%, rate %d milliHertz, ramp %d ms,"
                      " offset %d ms)",
                      ledContext->nowTick, msg->apply.led,
                      msg->levelAveragePercent,
                      msg->levelAmplitudePercent,
                      msg->rateMilliHertz,
                      msg->rampMs,
                      msg->apply.offsetLeftToRightMs);

    // Lock the LED context
    ledContext->mutex.lock();

    if (msg->apply.led < W_UTIL_ARRAY_COUNT(ledContext->ledState)) {
        // We're updating one LED
        wLedState_t *state = &(ledContext->ledState[msg->apply.led]);
        W_LOG_DEBUG_MORE("; %s LED mode %d, level %d%%, last change %06lld",
                         gLedStr[msg->apply.led], state->modeType,
                         state->levelAveragePercent, state->lastChangeTick);
        msgHandlerLedModeBreatheUpdate(msg->apply.led, state, ledContext->nowTick, msg);
    } else {
        // Update both LEDs
        for (size_t x = 0; x < W_UTIL_ARRAY_COUNT(ledContext->ledState); x++) {
            wLedState_t *state = &(ledContext->ledState[x]);
            W_LOG_DEBUG_MORE("; %s LED mode %d, level %d%%, last change %06lld",
                             gLedStr[x], state->modeType,
                             state->levelAveragePercent, state->lastChangeTick);
            msgHandlerLedModeBreatheUpdate((wLed_t) x, state, ledContext->nowTick, msg);
        }
    }
    W_LOG_DEBUG_MORE(".");
    W_LOG_DEBUG_END;

    // Unlock the context again
    ledContext->mutex.unlock();
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wLedMsgBodyOverlayMorse_t
 * -------------------------------------------------------------- */

// Message handler for wLedMsgBodyOverlayMorse_t.
static void msgHandlerLedOverlayMorse(void *msgBody, unsigned int bodySize,
                                      void *context)
{
    wLedMsgBodyOverlayMorse_t *msg = &(((wLedMsgBody_t *) msgBody)->overlayMorse);
    wLedContext_t *ledContext = (wLedContext_t *) context;
    assert(bodySize == sizeof(*msg));

    // TODO
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wLedMsgBodyOverlayWink_t
 * -------------------------------------------------------------- */

// Message handler for wLedMsgBodyOverlayWink_t.
static void msgHandlerLedOverlayWink(void *msgBody, unsigned int bodySize,
                                     void *context)
{
    wLedMsgBodyOverlayWink_t *msg = &(((wLedMsgBody_t *) msgBody)->overlayWink);
    wLedContext_t *ledContext = (wLedContext_t *) context;
    assert(bodySize == sizeof(*msg));

    // TODO
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wLedMsgBodyOverlayRandomBlink_t
 * -------------------------------------------------------------- */

// Message handler for wLedMsgBodyOverlayRandomBlink_t.
static void msgHandlerLedOverlayRandomBlink(void *msgBody,
                                            unsigned int bodySize,
                                            void *context)
{
    wLedMsgBodyOverlayRandomBlink_t *msg = &(((wLedMsgBody_t *) msgBody)->overlayRandomBlink);
    wLedOverlayRandomBlink_t *overlay = &(msg->overlay);
    wLedContext_t *ledContext = (wLedContext_t *) context;
    assert(bodySize == sizeof(*msg));

    if (overlay->intervalTicks > 0) {
        W_LOG_DEBUG("HANDLER [%06lld]: wLedMsgBodyOverlayRandomBlink_t"
                    " (rate %lld per minute, range %lld seconds, duration %lld ms).",
                    ledContext->nowTick,
                    (60 * 1000) / ticksToMs(overlay->intervalTicks),
                    ticksToMs(overlay->rangeTicks) / 1000,
                    ticksToMs(overlay->durationTicks));
    } else {
        W_LOG_DEBUG("HANDLER [%06lld]: wLedMsgBodyOverlayRandomBlink_t (blink off).");
    }
 
    // Lock the LED context
    ledContext->mutex.lock();

    if (overlay->intervalTicks == 0) {
        if (ledContext->randomBlink) {
            free(ledContext->randomBlink);
            ledContext->randomBlink = nullptr;
        }
    } else {
        if (!ledContext->randomBlink) {
            ledContext->randomBlink = (wLedOverlayRandomBlink_t *) malloc(sizeof(wLedOverlayRandomBlink_t));
        }
        if (ledContext->randomBlink) {
            wLedOverlayRandomBlink_t *randomBlink = ledContext->randomBlink;
            *randomBlink = *overlay;
            // Adding rangeTicks / 2 here to avid underrun in randomBlink() 
            randomBlink->lastBlinkTicks = ledContext->nowTick + (randomBlink->rangeTicks / 2);
        } else {
            W_LOG_ERROR("unable to allocate %d byte(s) for random blink!",
                        sizeof(wLedOverlayRandomBlink_t));
        }
    }

    // Unlock the context again
    ledContext->mutex.unlock();
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wLedMsgBodyLevelScale_t
 * -------------------------------------------------------------- */

// Message handler for wLedMsgBodyLevelScale_t.
static void msgHandlerLedLevelScale(void *msgBody, unsigned int bodySize,
                                    void *context)
{
    wLedMsgBodyLevelScale_t *msg = &(((wLedMsgBody_t *) msgBody)->levelScale);
    wLedContext_t *ledContext = (wLedContext_t *) context;
    assert(bodySize == sizeof(*msg));

    // TODO
    (void) ledContext;
}

/* ----------------------------------------------------------------
 * MORE VARIABLES: THE MESSAGES WITH THEIR MESSAGE HANDLERS
 * -------------------------------------------------------------- */

// Array of message handlers with the message type they handle.
static wLedMsgHandler_t gMsgHandler[] = {{.msgType = W_LED_MSG_TYPE_MODE_CONSTANT,
                                          .function = msgHandlerLedModeConstant},
                                         {.msgType = W_LED_MSG_TYPE_MODE_BREATHE,
                                         .function = msgHandlerLedModeBreathe},
                                         {.msgType = W_LED_MSG_TYPE_OVERLAY_MORSE,
                                          .function = msgHandlerLedOverlayMorse},
                                         {.msgType = W_LED_MSG_TYPE_OVERLAY_WINK,
                                          .function = msgHandlerLedOverlayWink},
                                         {.msgType = W_LED_MSG_TYPE_OVERLAY_RANDOM_BLINK,
                                          .function = msgHandlerLedOverlayRandomBlink},
                                         {.msgType = W_LED_MSG_TYPE_LEVEL_SCALE,
                                          .function = msgHandlerLedLevelScale}};

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise LEDs; starts ledLoop(), message queue and registers
// message handlers.
int wLedInit()
{
    int errorCode = 0;

    if (gTimerFd < 0) {
        // Create our message queue
        errorCode = wMsgQueueStart(&gContext, W_LED_MSG_QUEUE_MAX_SIZE_LED, "LED msg");
        if (errorCode >= 0) {
            gMsgQueueId = errorCode;
            errorCode = 0;
            // Register our message handlers
            for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMsgHandler)) &&
                                     (errorCode == 0); x++) {
                wLedMsgHandler_t *handler = &(gMsgHandler[x]);
                errorCode = wMsgQueueHandlerAdd(gMsgQueueId,
                                                handler->msgType,
                                                handler->function);
            }
            if (errorCode != 0) {
                // Tidy up on error
                 wMsgQueueStop(gMsgQueueId);
                 gMsgQueueId = -1;
            }
        }
        if (errorCode == 0) {
            // Set up a tick to drive ledLoop()
            errorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (errorCode >= 0) {
                struct itimerspec timerSpec = {};
                timerSpec.it_value.tv_nsec = W_LED_TICK_TIMER_PERIOD_MS * 1000000;
                timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
                if (timerfd_settime(errorCode, 0, &timerSpec, nullptr) == 0) {
                    gTimerFd = errorCode;
                    errorCode = 0;
                    // Start the LED loop
                    try {
                        // This will go bang if the thread cannot be created
                        gKeepGoing = true;
                        gContext.thread = std::thread(ledLoop);
                    }
                    catch (int x) {
                        // Close the timer on error
                        gKeepGoing = false;
                        close(gTimerFd);
                        gTimerFd = -1;
                        errorCode = -x;
                        W_LOG_ERROR("unable to start LED tick thread, error code %d.",
                                    errorCode);
                    }
                } else {
                    // Close the timer on error
                    close(errorCode);
                    errorCode = -errno;
                    W_LOG_ERROR("unable to set LED tick timer, error code %d.",
                                errorCode);
                }
            } else {
                errorCode = -errno;
                W_LOG_ERROR("unable to create LED tick timer, error code %d.",
                            errorCode);
            }
            if (errorCode != 0) {
                // Tidy up on error
                 wMsgQueueStop(gMsgQueueId);
                 gMsgQueueId = -1;
            }
        }
    }

    return errorCode;
}

// Deinitialise LEDs; stops ledLoop() and free's resources.
void wLedDeinit()
{
    if (gTimerFd >= 0) {
        if (gMsgQueueId >= 0) {
             wMsgQueueStop(gMsgQueueId);
             gMsgQueueId = -1;
        }
        gKeepGoing = false;
        if (gContext.thread.joinable()) {
            gContext.thread.join();
        }
        close(gTimerFd);
        gTimerFd = -1;
        if (gContext.randomBlink) {
            free(gContext.randomBlink);
        }
    }
}

// Set LED mode constant.
int wLedModeConstantSet(wLed_t led, int offsetLeftToRightMs,
                        unsigned int levelPercent,
                        unsigned int rampMs)
{
    int errorCode = -EBADF;
    wLedMsgBodyModeConstant_t msg = {};

    if (gMsgQueueId >= 0) {
        msg.apply.led = led;
        msg.apply.offsetLeftToRightMs = offsetLeftToRightMs;
        msg.levelPercent = levelPercent;
        msg.rampMs = rampMs;
        errorCode = wMsgPush(gMsgQueueId,
                             W_LED_MSG_TYPE_MODE_CONSTANT,
                             &msg, sizeof(msg));
        if (errorCode >= 0) {
            errorCode = 0;
        }
    }

    return errorCode;
}

// Set LED mode breathe.
int wLedModeBreatheSet(wLed_t led, int offsetLeftToRightMs,
                       unsigned int rateMilliHertz,
                       unsigned int levelAveragePercent,
                       unsigned int levelAmplitudePercent,
                       unsigned int rampMs)
{
    int errorCode = -EBADF;
    wLedMsgBodyModeBreathe_t msg = {};

    if (gMsgQueueId >= 0) {
        msg.apply.led = led;
        msg.apply.offsetLeftToRightMs = offsetLeftToRightMs;
        msg.rateMilliHertz = rateMilliHertz;
        msg.levelAveragePercent = levelAveragePercent;
        msg.levelAmplitudePercent = levelAmplitudePercent;
        msg.rampMs = rampMs;
        errorCode = wMsgPush(gMsgQueueId,
                             W_LED_MSG_TYPE_MODE_BREATHE,
                             &msg, sizeof(msg));
        if (errorCode >= 0) {
            errorCode = 0;
        }
    }

    return errorCode;
}

// Set a Morse overlay.
int wLedOverlayMorseSet(wLed_t led, int offsetLeftToRightMs,
                        const char *sequenceStr,
                        unsigned int repeat,
                        unsigned int levelPercent,
                        unsigned int durationDotMs,
                        unsigned int durationDashMs,
                        unsigned int durationGapLetterMs,
                        unsigned int durationGapWordMs,
                        unsigned int durationGapRepeatMs)
{
    int errorCode = -EBADF;
    wLedMsgBodyOverlayMorse_t msg = {};
    wLedOverlayMorse_t *overlay = &(msg.overlay);

    if (gMsgQueueId >= 0) {
        errorCode = -EINVAL;
        if (strlen(sequenceStr) < sizeof(overlay->sequenceStr)) {
            msg.apply.led = led;
            msg.apply.offsetLeftToRightMs = offsetLeftToRightMs;
            strcpy(overlay->sequenceStr, sequenceStr);
            overlay->repeat = repeat;
            overlay->levelPercent = levelPercent;
            overlay->durationDotTicks = msToTicks(durationDotMs);
            overlay->durationDashTicks = msToTicks(durationDashMs);
            overlay->durationGapLetterTicks = msToTicks(durationGapLetterMs);
            overlay->durationGapWordTicks = msToTicks(durationGapWordMs);
            overlay->durationGapRepeatTicks = msToTicks(durationGapRepeatMs);
            errorCode = wMsgPush(gMsgQueueId,
                                 W_LED_MSG_TYPE_OVERLAY_MORSE,
                                 &msg, sizeof(msg));
            if (errorCode >= 0) {
                errorCode = 0;
            }
        }
    }

    return errorCode;
}

// Set a wink overlay.
int wLedModeOverlayWinkSet(wLed_t led, unsigned int durationMs)
{
    int errorCode = -EBADF;
    wLedMsgBodyOverlayWink_t msg = {};
    wLedOverlayWink_t *overlay = &(msg.overlay);

    if (gMsgQueueId >= 0) {
        msg.apply.led = led;
        overlay->durationTicks = msToTicks(durationMs);
        errorCode = wMsgPush(gMsgQueueId,
                             W_LED_MSG_TYPE_OVERLAY_WINK,
                             &msg, sizeof(msg));
        if (errorCode >= 0) {
            errorCode = 0;
        }
    }

    return errorCode;
}

// Set a random blink overlay.
int wLedModeOverlayRandomBlinkSet(unsigned int ratePerMinute,
                                  int rangeSeconds,
                                  unsigned int durationMs)
{
    int errorCode = -EBADF;
    wLedMsgBodyOverlayRandomBlink_t msg = {};
    wLedOverlayRandomBlink_t *overlay = &(msg.overlay);

    if (gMsgQueueId >= 0) {
        if (ratePerMinute > 0) {
            overlay->intervalTicks = msToTicks(60 * 1000 / ratePerMinute);
        }
        overlay->rangeTicks =  msToTicks(rangeSeconds * 1000);
        overlay->durationTicks = msToTicks(durationMs);
        errorCode = wMsgPush(gMsgQueueId,
                             W_LED_MSG_TYPE_OVERLAY_RANDOM_BLINK,
                             &msg, sizeof(msg));
        if (errorCode >= 0) {
            errorCode = 0;
        }
    }

    return errorCode;
}

// Scale the level of one or both LEDs.
int wLedModeLevelScaleSet(wLed_t led, unsigned int percent,
                          unsigned int rampMs)
{
    int errorCode = -EBADF;
    wLedMsgBodyLevelScale_t msg = {};

    if (gMsgQueueId >= 0) {
        msg.apply.led = led;
        msg.apply.offsetLeftToRightMs = 0;
        msg.percent = percent;
        msg.rampMs = rampMs;
        errorCode = wMsgPush(gMsgQueueId,
                             W_LED_MSG_TYPE_LEVEL_SCALE,
                             &msg, sizeof(msg));
        if (errorCode >= 0) {
            errorCode = 0;
        }
    }

    return errorCode;
}

// Run through a test sequence for the LEDs: everything must already
// have been initialised before this can be called.
int wLedTest()
{
    int errorCode = 0;
    const char *prefix = "LED TEST: ";

    W_LOG_INFO("%sSTART (will take a little while).", prefix);

    if (wUtilKeepGoing()) {
        W_LOG_INFO("%sboth LEDs on at 100%.", prefix);
        errorCode = wLedModeConstantSet(W_LED_BOTH, 0, 100, 3000);
        if (errorCode == 0) {
            sleep(5);
        }
    }

    if ((errorCode == 0) && wUtilKeepGoing()) {
        W_LOG_INFO("%stesting blinking for 15 seconds.", prefix);
        errorCode = wLedModeOverlayRandomBlinkSet(10, 2);
        if (errorCode == 0) {
            sleep(15);
        }
        // Switch blinking off again
        errorCode = wLedModeOverlayRandomBlinkSet(0);
    }

    if ((errorCode == 0) && wUtilKeepGoing()) {
        // Switch both LEDs off between tests
        errorCode = wLedModeConstantSet(W_LED_BOTH, 0, 0);
        if (errorCode == 0) {
            sleep(2);

            W_LOG_INFO("%stesting breathe mode.", prefix);
            W_LOG_INFO("%sboth LEDs ramped up, left ahead of right.",
                       prefix);
            errorCode = wLedModeBreatheSet(W_LED_BOTH, 1000, 1000, 50, 50, 1000);
            if (errorCode == 0) {
                sleep(5);
            }
        }
    }

    if ((errorCode == 0) && wUtilKeepGoing()) {
        W_LOG_INFO("%sLEDs in sync now.",  prefix);
        errorCode = wLedModeBreatheSet(W_LED_BOTH, 0, 1000, 50, 50, 1000);
        if (errorCode == 0) {
            sleep(5);
        }
    }

    if ((errorCode == 0) && wUtilKeepGoing()) {
        W_LOG_INFO("%s%s LED ramped down, but with smaller amplitude and faster.",
                   prefix, gLedStr[W_LED_LEFT]);
        errorCode = wLedModeBreatheSet(W_LED_LEFT, 0, 2000, 0, 15, 5000);
        if (errorCode == 0) {
            sleep(5);
            // Switch the left LED off
            errorCode = wLedModeBreatheSet(W_LED_LEFT, 0, 1000, 0, 0);
            if (errorCode == 0) {
                sleep(1);
            }
        }
    }
    if ((errorCode == 0) && wUtilKeepGoing()) {
        W_LOG_INFO("%s%s LED ramped down, but with larger amplitude and slower.",
                   prefix, gLedStr[W_LED_RIGHT]);
        errorCode = wLedModeBreatheSet(W_LED_RIGHT, 0, 500, 0, 70, 5000);
        if (errorCode == 0) {
            sleep(5);
            // Switch the right LED off
            errorCode = wLedModeBreatheSet(W_LED_RIGHT, 0, 1000, 0, 0);
            if (errorCode == 0) {
                sleep(1);
            }
        }
    }
    if ((errorCode == 0) && wUtilKeepGoing()) {
        // Switch both LEDs off between tests
        errorCode = wLedModeConstantSet(W_LED_BOTH, 0, 0);
        if (errorCode == 0) {
            sleep(2);
        }
    }

    if ((errorCode == 0) && wUtilKeepGoing()) {
        W_LOG_INFO("%stesting constant mode.", prefix);
        W_LOG_INFO("%sboth LEDs ramped up over one second, left ahead of right.",
                   prefix);
        errorCode = wLedModeConstantSet(W_LED_BOTH, 1000, 100, 1000);
        if (errorCode == 0) {
            sleep(2);
            W_LOG_INFO("%s%s LED ramped down.", prefix, gLedStr[W_LED_LEFT]);
            errorCode = wLedModeConstantSet(W_LED_LEFT, 0, 0, 1000);
            if (errorCode == 0) {
                sleep(2);
                W_LOG_INFO("%s%s LED ramped down.", prefix, gLedStr[W_LED_RIGHT]);
                errorCode = wLedModeConstantSet(W_LED_RIGHT, 0, 0, 1000);
                if (errorCode == 0) {
                    sleep(2);
                    // Switch both LEDs off at the end
                    errorCode = wLedModeConstantSet(W_LED_BOTH, 0, 0);
                    if (errorCode == 0) {
                        sleep(1);
                    }
                }
            }
        }
    }

    W_LOG_INFO("%scompleted.", prefix);

    return errorCode;
}

// End of file