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
/** The control tick timer period in milliseconds.  If you change
 * this you may also need to change
 * W_CONTROL_FOCUS_MOVE_INTERVAL_TICKS below.  Cannot be too
 * fast as we will be performing motor movements in the
 * control loop that this tick drives: 20 ms or more is good.
 */
# define W_CONTROL_TICK_TIMER_PERIOD_MS 50
#endif

#ifndef W_CONTROL_FOCUS_MOVE_INTERVAL_TICKS
/** The number of control ticks to wait after movement.
 */
# define W_CONTROL_FOCUS_MOVE_INTERVAL_TICKS 20
#endif

#ifndef W_CONTROL_FOCUS_ROTATE_INCREMENT_STEPS
/** The amount to move W_MOTOR_TYPE_ROTATE each time in steps.
 */
# define W_CONTROL_FOCUS_ROTATE_INCREMENT_STEPS 1
#endif

#ifndef W_CONTROL_FOCUS_VERTICAL_INCREMENT_STEPS
/** The amount to move WMOTOR_TYPE_VERTICAL each time in steps.
 */
# define W_CONTROL_FOCUS_VERTICAL_INCREMENT_STEPS 1
#endif

#ifndef W_CONTROL_FOCUS_AVERAGE_LENGTH
/** The number of focus points to average over; for instance,
 * 15, with a frame rate of 15, would give an average over at
 * least one second.
 */
# define W_CONTROL_FOCUS_AVERAGE_LENGTH 15
#endif

#ifndef W_CONTROL_FOCUS_THRESHOLD_AREA_PIXELS
/** The minimum size of a focus area that should cause us to
 * pay any attention, in pixels.
 */
# define W_CONTROL_FOCUS_THRESHOLD_AREA_PIXELS 100
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise the control loop; if control is already initialised
 * this function will do nothing and return success. wMsgInit()
 * must have returned successfully before this is called.
 *
 * @return zero on success else negative error code.
 */
int wControlInit();

/** Start control operations; wImageProcessingInit() and
 * wVideoEncodeInit() must have been called and returned success
 * for this function to succeed.
 *
 * @param staticCamera don't move the camera (noting that this does
 *                     not affect calibration, which wMotorInit()
 *                     will always carry out).
 * @return             zero on success else negative error code.
 */
int wControlStart(bool staticCamera = false);

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
