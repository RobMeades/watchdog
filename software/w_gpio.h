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

#ifndef _W_GPIO_H_
#define _W_GPIO_H_

/** @file
 * @brief The API to the GPIO portion of the watchdog application.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT
/** The GPIO input pin that detects the state of the "look left limit"
 * switch as one is standing behind the watchdog, looking out of
 * the watchdog's eyes, i.e. it is the limit switch on the _right_
 * side of the collar as one is standing behind the watchdog
 * looking forward.
 */
# define W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT 1
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT
/** The GPIO input pin that detects the state of the "look right limit"
 * switch as one is standing behind the watchdog, looking out of
 * the watchdog's eyes, i.e. it is the limit switch on the _left_
 * side of the collar as one is standing behind the watchdog
 * looking forward.
 */
# define W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT 2
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT
/** The GPIO input pin detecting the state of the "look down limit", i.e.
 * the switch on the front of the watchdog's body.
 */
# define W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT 3
#endif

#ifndef W_GPIO_PIN_INPUT_LOOK_UP_LIMIT
/** The GPIO input pin detecting the state of the "look up limit", i.e.
 * the switch on the rear of the watchdog's body.
 */
# define W_GPIO_PIN_INPUT_LOOK_UP_LIMIT 4
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_DISABLE
/** The GPIO output pin that enables the stepper motor that rotates the
 * watchdog's head.
 * NOTE: the pin on the Sparkfun board is labelled "enable" but a logic
 * 1 disables, a logic 0 enables, so it might better be called "enable bar"
 * or more clearly DISABlE
 */
# define W_GPIO_PIN_OUTPUT_ROTATE_DISABLE 5
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION
/** The GPIO output pin that sets the direction of rotation: 1 for clock-wise,
 * 0 for anti-clockwise.
 */
# define W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION 6
#endif

#ifndef W_GPIO_PIN_OUTPUT_ROTATE_STEP
/** The GPIO output pin that, when pulsed, causes the rotation stepper motor
 * to move one step.
 */
# define W_GPIO_PIN_OUTPUT_ROTATE_STEP 7
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE
/** The GPIO output pin that enables the stepper motor that lowers and
 * raises the watchdog's head.
 * NOTE: the pin on the Sparkfun board is labelled "enable" but a logic
 * 1 disables, a logic 0 enables, so it might better be called "enable bar"
 * or more clearly DISABlE
 */
# define W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE 8
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION
/** The GPIO output pin that sets the direction of vertical motion: 1 for,
 * down, 0 for up.
 */
# define W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION 9
#endif

#ifndef W_GPIO_PIN_OUTPUT_VERTICAL_STEP
/** The GPIO output pin that, when pulsed, causes the vertical stepper motor
 * to move one step.
 */
# define W_GPIO_PIN_OUTPUT_VERTICAL_STEP 10
#endif

#ifndef W_GPIO_PIN_OUTPUT_EYE_LEFT
/** The GPIO pin driving the LED in the left eye of the watchdog.
 */
# define W_GPIO_PIN_OUTPUT_EYE_LEFT 12
#endif

#ifndef W_GPIO_PIN_OUTPUT_EYE_RIGHT
/** The GPIO pin driving the LED in the right eye of the watchdog.
 */
# define W_GPIO_PIN_OUTPUT_EYE_RIGHT 13
#endif

#ifndef W_GPIO_CHIP_NUMBER
/** The number of the GPIO chip to use: 0 for a Pi 5's header pins.
 */
# define W_GPIO_CHIP_NUMBER 0
#endif

#ifndef W_GPIO_CONSUMER_NAME
/** A string to identify us as a consumer of a GPIO pin.
 */
# define W_GPIO_CONSUMER_NAME "watchdog"
#endif

#ifndef W_GPIO_DEBOUNCE_THRESHOLD
/** The number of times an input pin must have read a consistently
 * different level to the current level for us to believe that it
 * really has changed state.  With a GPIO tick timer of 1 ms and
 * four input pins, that means that the pin must have read the same
 * level for a full 12 milliseconds before we believe it.
 */
# define W_GPIO_DEBOUNCE_THRESHOLD 3
#endif

#ifndef W_GPIO_TICK_TIMER_PERIOD_US
/** The GPIO tick timer period in microseconds: this is used to
 * debounce the input pins, reading one each time, so will
 * run around the four inputs once every fourth period, so using
 * 1 millisecond here would mean an input line is read every
 * 4 milliseconds and so a W_GPIO_DEBOUNCE_THRESHOLD of 3 would
 * mean that a GPIO would need to have read a consistent level
 * for 12 milliseconds.
 */
# define W_GPIO_TICK_TIMER_PERIOD_US 1000
#endif

#ifndef W_GPIO_PWM_TIMER_PERIOD_US
/** The GPIO PWM timer period in microseconds: used to drive PWM
 * where, since we don't have a capacitor on the LED, we drive
 * at quite a high rate.
 */
# define W_GPIO_PWM_TIMER_PERIOD_US 1000
#endif

#ifndef W_GPIO_PWM_MAX_COUNT
/** The number of PWM timer intervals that represents 100%.  With
 * a PWM timer period of 1 ms, using 20 here keeps the flicker rate
 * down to a non-visible 20 ms.
 */
# define W_GPIO_PWM_MAX_COUNT 20
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise the GPIO pins; if the GPIO pins were already
 * initialised this function will do nothing and return success.
 *
 * @return    zero on success else negative error code.
 */
int wGpioInit();

/** Deinitialise the GPIO pins.
 */
void wGpioDeinit();

/** Get the state of a GPIO pin after debouncing.
 *
 * @param pin the GPIO pin number to get the state of.
 * @return    the level of the GPIO pin else negative error code.
 */
int wGpioGet(unsigned int pin);

/** Set the state of a GPIO pin.
 *
 * @param pin   the GPIO pin number to set the state of.
 * @param level the level to set.
 * @return      zero on success else negative error code.
 */
int wGpioSet(unsigned int pin, unsigned int level);

/** Set the state of a GPIO pin.
 *
 * @param pin          the GPIO pin number to set the state of.
 * @param levelPercent the level to set, as a percentage.
 * @return             zero on success else negative error code.
 */
int wGpioPwmSet(unsigned int pin, unsigned int levelPercent);

#endif  // _W_GPIO_H_

// End of file
