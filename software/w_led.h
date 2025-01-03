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

#ifndef _W_LED_H_
#define _W_LED_H_

/** @file
 * @brief The LED control API for the watchdog application;
 * this API is thread-safe aside from wLedInit() and wLedDeinit(),
 * which should not be called at the same time as any other API or
 * each other.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_LED_MSG_QUEUE_MAX_SIZE_LED
/** The maximum number of message in the LED message queue.
 */
# define W_LED_MSG_QUEUE_MAX_SIZE_LED 10
#endif

#ifndef W_LED_TICK_TIMER_PERIOD_MS
/** The LED tick timer period in milliseconds.  If you change
 * this you may also need to change W_LED_MORSE_DURATION_UNIT_MS
 * below.
 */
# define W_LED_TICK_TIMER_PERIOD_MS 20
#endif

#ifndef W_LED_MORSE_MAX_SIZE
/** The maximum length of a morse message to be flashed by an
 * LED, including room for a null terminator.
 */
# define W_LED_MORSE_MAX_SIZE (128 + 1)
#endif

#ifndef W_LED_MORSE_DURATION_UNIT_MS
/** The default unit duration when an LED is flashing morse,
 * in milliseconds.  From
 * https://en.wikipedia.org/wiki/Morse_code#/media/File:International_Morse_Code.svg,
 * a dot is one unit, a dash is three units, the space between
 * the dots and dashes within a letter is one unit, the space
 * between letters is three units and the space between  words is
 * seven units.
 *
 * The duration is chosen as a multiple of W_LED_TICK_TIMER_PERIOD_MS.
 */
# define W_LED_MORSE_DURATION_UNIT_MS (W_LED_TICK_TIMER_PERIOD_MS * 10)
#endif

#ifndef W_LED_MORSE_DURATION_MULTIPLIER_DOT
/** The multiplier of W_LED_MORSE_DURATION_UNIT_MS to form a
 * dot.
 */
# define W_LED_MORSE_DURATION_MULTIPLIER_DOT 1
#endif

#ifndef W_LED_MORSE_DURATION_MULTIPLIER_DASH
/** The multiplier of W_LED_MORSE_DURATION_UNIT_MS to form a
 * dash.
 */
# define W_LED_MORSE_DURATION_MULTIPLIER_DASH 3
#endif

#ifndef W_LED_MORSE_DURATION_MULTIPLIER_GAP
/** The multiplier of W_LED_MORSE_DURATION_UNIT_MS to form a
 * gap between the dots and dashes within a letter.
 */
# define W_LED_MORSE_DURATION_MULTIPLIER_GAP 1
#endif

#ifndef W_LED_MORSE_DURATION_MULTIPLIER_GAP_LETTER
/** The multiplier of W_LED_MORSE_DURATION_UNIT_MS to form a
 * gap between letters.
 */
# define W_LED_MORSE_DURATION_MULTIPLIER_GAP_LETTER 3
#endif

#ifndef W_LED_MORSE_DURATION_MULTIPLIER_GAP_WORD
/** The multiplier of W_LED_MORSE_DURATION_UNIT_MS to form a
 * gap between words.  Since there will be a letter gap after
 * the last letter of a word, this is only 4 rather than 7.
 */
# define W_LED_MORSE_DURATION_MULTIPLIER_GAP_WORD 4
#endif

#ifndef W_LED_MORSE_DURATION_GAP_REPEAT_MS
/** The default duration of a gap between repeats when an LED is
 * flashing morse repeatedly, in milliseconds.
 */
# define W_LED_MORSE_DURATION_GAP_REPEAT_MS 500
#endif

#ifndef W_LED_WINK_DURATION_MS
/** The default duration of a wink in milliseconds.
 */
# define W_LED_WINK_DURATION_MS 250
#endif

#ifndef W_LED_RANDOM_BLINK_RATE_PER_MINUTE
/** The default LED random blink rate per minute.
 */
# define W_LED_RANDOM_BLINK_RATE_PER_MINUTE 1
#endif

#ifndef W_LED_RANDOM_BLINK_DURATION_MS
/** The default duration of a blink in milliseconds.
 */
# define W_LED_RANDOM_BLINK_DURATION_MS 100
#endif

#ifndef W_LED_RANDOM_BLINK_RANGE_SECONDS
/** The default range of variation on a random blink interval
 * in seconds.
 */
# define W_LED_RANDOM_BLINK_RANGE_SECONDS 10
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Identify the LEDs; order is important, the first two must match
 * the order of the LED pins in gLedToPin[].
 */
typedef enum {
    W_LED_LEFT = 0,
    W_LED_RIGHT = 1,
    W_LED_MAX_NUM,
    W_LED_BOTH = W_LED_MAX_NUM
} wLed_t;

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise LEDs; note that wMsgInit() must have already been
 * called for this to return successfully.  If the LEDs have already
 * been initialised this function will do nothing and return success.
 * wGpioInit() and wMsgInit() must have returned successfully before
 * this is called.
 *
 * @return zero on success else negative error code.
 */
int wLedInit();

/** Set LED mode to a constant brightness.
 *
 * @param led                  the LED or LEDs to set to constant mode.
 * @param offsetLeftToRightMs  the offset from the left to the right
 *                             LED in milliseconds (negative for the
 *                             other way); only employed if led is
 *                             W_LED_BOTH.
 * @param levelPercent         the brightness to apply as a percentage.
 * @param rampMs               the time allowed to ramp from the current
 *                             level to the new level in milliseconds.
 * @return                     zero on success else negative error code.
 */
int wLedModeConstantSet(wLed_t led = W_LED_BOTH,
                        int offsetLeftToRightMs = 0,
                        unsigned int levelPercent = 100,
                        unsigned int rampMs = 0);

/** Set LED to a rhythmic "breathe" mode.
 *
 * @param led                   the LED or LEDs to set to breathe mode.
 * @param offsetLeftToRightMs   the offset from the left to the right
 *                              LED in milliseconds (negative for the
 *                              other way); only employed if led is
 *                              W_LED_BOTH.
 * @param rateMilliHertz        the rate to breathe at in milli-Hertz.
 * @param levelAveragePercent   the average brightness as a percentage.
 * @param levelAmplitudePercent the amplitude of the brightness variation
 *                              as a percentage.
 * @param rampMs                the time allowed to ramp from the current
 *                              level to the new level in milliseconds.
 * @return                      zero on success else negative error code.
 */
int wLedModeBreatheSet(wLed_t led = W_LED_BOTH,
                       int offsetLeftToRightMs = 0,
                       unsigned int rateMilliHertz = 1000,
                       unsigned int levelAveragePercent = 50,
                       unsigned int levelAmplitudePercent = 50,
                       unsigned int rampMs = 0);

/** Add a Morse code sequence as an overlay to the current mode, replacing
 * the mode for the duration of the morse sequence times the number of
 * repeats.
 *
 * @param led                   the LED or LEDs apply the overlay to.
 * @param sequenceStr           the null-terminated sequence of characters
 *                              in the morse sequence, max length
 *                              W_LED_MORSE_MAX_SIZE (including null
 *                              terminator).  Lower case characters will
 *                              be converted to upper case, any
 *                              characters outside A to Z and 0 to 9 will
 *                              be ignored.  Use nullptr to clear a previous
 *                              morse overlay prematurely.
 * @param repeat                the number of times to repeat the morse
 *                              sequence; 0 means just do it once.
 * @param levelPercent          the brightness to apply as a percentage.
 * @param durationUnitMs        the duration of a unit in milliseconds,
 *                              where, from
 *                              https://en.wikipedia.org/wiki/Morse_code#/media/File:International_Morse_Code.svg,
 *                              a dot is one unit, a dash is three units,
 *                              the space between the dots and dashes within
 *                              a letter is one unit, the space between
 *                              letters is three units and the space between
 *                              words is seven units.
 * @param durationGapRepeatMs   the duration of the gap between repeats of
 *                              a morse sequence in milliseconds.
 * @return                      zero on success else negative error code.
 */
int wLedOverlayMorseSet(wLed_t led = W_LED_BOTH,
                        const char *sequenceStr = nullptr,
                        unsigned int repeat = 0,
                        unsigned int levelPercent = 100,
                        unsigned int durationUnitMs = W_LED_MORSE_DURATION_UNIT_MS,
                        unsigned int durationGapRepeatMs = W_LED_MORSE_DURATION_GAP_REPEAT_MS);

/** Add a wink overlay (i.e. an LED switching off for a brief period)
 * as an overlay to the current mode.
 *
 * @param led                   the LED to wink.
 * @param durationMs            the duration of the blink in milliseconds.
 * @return                      zero on success else negative error code.
 */
int wLedOverlayWinkSet(wLed_t led,
                       unsigned int durationMs = W_LED_WINK_DURATION_MS);

/** Add a random blink overlay (i.e. both LEDs switching off for a brief period)
 * as an overlay to the current mode.
 *
 * @param ratePerMinute  the number of blinks per minute, on average.
 * @param rangeSeconds   the timing variation between blinks in seconds.
 * @param durationMs     the duration of the wink in milliseconds.
 * @return               zero on success else negative error code.
 */
int wLedOverlayRandomBlinkSet(unsigned int ratePerMinute = W_LED_RANDOM_BLINK_RATE_PER_MINUTE,
                              int rangeSeconds = W_LED_RANDOM_BLINK_RANGE_SECONDS,
                              unsigned int durationMs = W_LED_RANDOM_BLINK_DURATION_MS);

/** Scale the brightness of one or both LEDs; note that this does not
 * apply to the Morse overlay, which always has maximum brightness
 * for readability.
 *
 * @param led         the LED to apply the scale factor to.
 * @param percent     the scale factor as a percentage.
 * @param rampMs      the time allowed to ramp from the current
 *                    level to the new level in milliseconds.
 * @return            zero on success else negative error code.
 */
int wLedLevelScaleSet(wLed_t led = W_LED_BOTH, unsigned int percent = 100,
                      unsigned int rampMs = 0);

/** Deinitialise LEDs and free resources.
 *
 * @return zero on success else negative error code.
 */
void wLedDeinit();

/** Run through a test sequence for the LEDs: wLedInit() must have
 * been called and returned successfully for this to work.
 *
 * @return zero on success else negative error code.
 */
int wLedTest();

#endif // _W_LED_H_

// End of file
