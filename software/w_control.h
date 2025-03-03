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

#ifndef _W_CONTROL_H_
#define _W_CONTROL_H_

// This API is dependent on w_common.h (W_COMMON_FRAME_RATE_HERTZ).
#include <w_common.h>

/** @file
 * @brief The control API for the watchdog application; this API is
 * NOT thread-safe.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_CONTROL_MSG_QUEUE_MAX_SIZE
/** The maximum number of messages allowed in the control queue;
 * shouldn't need many.
 */
# define W_CONTROL_MSG_QUEUE_MAX_SIZE 100
#endif

#ifndef W_CONTROL_TICK_TIMER_PERIOD_MS
/** The control tick-timer period in milliseconds.  If you change
 * this you may also need to change
 * W_CONTROL_FOCUS_MOVE_INTERVAL_TICKS below.  Cannot be too
 * fast as we will be stepping the motors forward in the
 * control loop that this tick drives, can't be too slow as we
 * want to move fairly smarly: 10 ms is good.
 */
# define W_CONTROL_TICK_TIMER_PERIOD_MS 10
#endif

#ifndef W_CONTROL_CFG_REFRESH_SECONDS
/** How often to check the configuration to see if the lights or
 * the motors should be off; once a second is good.
 */
# define W_CONTROL_CFG_REFRESH_SECONDS 1
#endif

#ifndef W_CONTROL_FOCUS_AVERAGE_LENGTH
/** The number of focus points to average over; for instance,
 * 15, with a frame rate of 15, would give an average over at
 * least one second.
 */
# define W_CONTROL_FOCUS_AVERAGE_LENGTH W_COMMON_FRAME_RATE_HERTZ
#endif

#ifndef W_CONTROL_FOCUS_AREA_THRESHOLD_PIXELS
/** The minimum size of a focus area that should cause us to
 * pay any attention, in pixels.
 */
# define W_CONTROL_FOCUS_AREA_THRESHOLD_PIXELS 100
#endif

#ifndef W_COMMON_FOCUS_CHANGE_THRESHOLD_PIXELS
/** How far from the origin the focus must be for us to move
 * towards it, in pixels.
 */
# define W_COMMON_FOCUS_CHANGE_THRESHOLD_PIXELS 50
#endif

#ifndef W_CONTROL_MOTOR_MOVE_GUARD_MS
/** A guard period to wait after moving, for things to
 * settle down before we take notice of focus changes again; needs
 * at least 5 seconds, otherwise the motion detection algorithm
 * tends to keep hold of static lines where there is a contrast
 * change (which of course are "moving" when the camera is moving).
 */
# define W_CONTROL_MOTOR_MOVE_GUARD_MS 5000
#endif

#ifndef W_CONTROL_MOTOR_MOVE_INTERVAL_MS
/** The minimum number of milliseconds to wait between movements;
 * this must be at least W_CONTROL_MOTOR_MOVE_GUARD_MS plus
 * W_CONTROL_FOCUS_AVERAGE_LENGTH in milliseconds.
 */
# define W_CONTROL_MOTOR_MOVE_INTERVAL_MS (W_CONTROL_MOTOR_MOVE_GUARD_MS + \
                                           ((W_CONTROL_FOCUS_AVERAGE_LENGTH * 1000) / W_COMMON_FRAME_RATE_HERTZ))
#endif

#ifndef W_CONTROL_MOVE_RAMP_PERCENT
/** The percentage of a move that should be spent in speeding
 * up at the start and down at the end; 30% is a good value
 * (15% at the start, 15% at the end).
 */
# define W_CONTROL_MOVE_RAMP_PERCENT 30
#endif

#ifndef W_CONTROL_LED_RAMP_UP_RATE_MS
/** Ramp-up rate for the LEDs, to make things look more organic.
 */
# define W_CONTROL_LED_RAMP_UP_RATE_MS 1000
#endif

#ifndef W_CONTROL_LED_RAMP_DOWN_RATE_MS
/** Ramp-down rate for the LEDs; this is separate to the ramp-up
 * rate as it needs to be longer to prevent the watchdog detecting
 * changes in the magnitude of the reflection of its own eyes, when
 * going idle, as movement, resulting in self-retriggering; 5 seconds
 * is good.
 */
# define W_CONTROL_LED_RAMP_DOWN_RATE_MS 5000
#endif

#ifndef W_CONTROL_LED_IDLE_PERCENT
/** The brightness of the LEDs when idle: jsut 10% is enough
 * with these ultra-bright ones.
 */
# define W_CONTROL_LED_IDLE_PERCENT 10
#endif

#ifndef W_CONTROL_LED_ACTIVE_PERCENT
/** The brightness of the LEDs when active.
 */
# define W_CONTROL_LED_ACTIVE_PERCENT 100
#endif

#ifndef W_CONTROL_LED_RANDOM_BLINK_RATE_PER_MINUTE
/** The blink rate per minute; by default a range
 * of up to 10 seconds will be added to this.
 */
# define W_CONTROL_LED_RANDOM_BLINK_RATE_PER_MINUTE 5
#endif

#ifndef W_CONTROL_STEP_INTERVAL_MAX_MS
/** The maximum interval between steps, applied at the start
 * of the ramping period (will be zero by the end of the ramping
 * period).
 */
# define W_CONTROL_STEP_INTERVAL_MAX_MS 100
#endif

#ifndef W_CONTROL_INACTIVITY_RETURN_TO_REST_SECONDS
/** After how many seconds to return to the "rest" position
 * due to inactivity; use 0 for no inactivity rest.
 */
# define W_CONTROL_INACTIVITY_RETURN_TO_REST_SECONDS 30
#endif

#ifndef W_CONTROL_RETURN_TO_REST_SECONDS
/** How often to return to the "rest" position periodically
 * in seconds;  use 0 to never return to the rest position
 * periodically.
 */
# define W_CONTROL_RETURN_TO_REST_SECONDS 0
#endif

#ifndef W_CONTROL_MOTOR_RECALIBRATE_SECONDS
/** How often to recalibrate the motors periodically in seconds;
 * use 0 to never recalibrate the motors periodically during
 * normal operation.
 */
# define W_CONTROL_MOTOR_RECALIBRATE_SECONDS 0
#endif

// Do some checking.
#if W_CONTROL_MOTOR_MOVE_INTERVAL_MS < (W_CONTROL_MOTOR_MOVE_GUARD_MS + ((W_CONTROL_FOCUS_AVERAGE_LENGTH * 1000) / W_COMMON_FRAME_RATE_HERTZ))
# error W_CONTROL_MOTOR_MOVE_INTERVAL_MS must be at least W_CONTROL_MOTOR_MOVE_GUARD_MS plus W_CONTROL_FOCUS_AVERAGE_LENGTH in milliseconds!
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise the control loop; if control is already initialised
 * this function will do nothing and return success.  wMsgInit()
 * must have returned successfully before this is called.
 *
 * @return zero on success else negative error code.
 */
int wControlInit();

/** Start control operations; wMotorInit(), wImageProcessingInit()
 * and wVideoEncodeInit() must have been called and returned success
 * for this function to succeed.
 *
 * Note that there is no normally no reason to set any of the
 * limit values; these will be determined automatically by this
 * API during calibration.  Only set a value if you have a particular
 * need to constrain movement in a direction.
 *
 * @param staticCamera            don't move the camera (noting that
 *                                this does  not affect calibration,
 *                                which wMotorInit() will always carry
 *                                out).
 * @param motionContinuousSeconds motion must have been occurring for
 *                                at least this number of seconds before
 *                                control will react to it; useful, for
 *                                instance, for ignoring fast moving objects
 *                                like cars.
 * @param lookUpLimitSteps        use this as the look-up limit for normal
 *                                operation, in stepper-motor steps; it
 *                                will be ignored during calibration, zero
 *                                means no limit, value is an offset from the
 *                                calibrated up/down centre.
 * @param lookDownLimitSteps      use this as the look-down limit for normal
 *                                operation, in stepper-motor steps; it
 *                                will be ignored during calibration, zero
 *                                means no limit, value is and offset from the
 *                                calibrated up/down centre.
 * @param lookRightLimitSteps     use this as the look-right limit for normal
 *                                operation, in stepper-motor steps; it
 *                                will be ignored during calibration, zero
 *                                means no limit, value is an offset from the
 *                                calibrated left/right centre.
 * @param lookLeftLimitSteps      use this as the look-left limit for normal
 *                                operation, in stepper-motor steps; it
 *                                will be ignored during calibration, zero
 *                                means no limit, value is an offset from the
 *                                calibrated left/right centre.
 * @param cfgIgnore               ignore what the cfg API is saying, i.e.
 *                                motors and lights are always on.
 * @return                        zero on success else negative error code.
 */
int wControlStart(bool staticCamera = false,
                  int motionContinuousSeconds = 0,
                  int lookUpLimitSteps = 0, int lookDownLimitSteps = 0,
                  int lookLeftLimitSteps = 0, int lookRightLimitSteps = 0,
                  bool cfgIgnore = false);

/** Stop control operations; you do not have to call this function
 * on exit, wControlDeinit() will tidy up appropriately.
 *
 * @return zero on success else negative error code.
 */
int wControlStop();

/** Deinitialise the control loop and free resources.
 */
void wControlDeinit();

#endif // _W_CONTROL_H_

// End of file
