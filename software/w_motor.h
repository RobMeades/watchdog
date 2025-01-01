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

#ifndef _W_MOTOR_H_
#define _W_MOTOR_H_

/** @file
 * @brief The motor API for the watchdog application; this API is
 * thread-safe aside from wMotorInit()/wMotorDeinit(), which should
 * not be called at the same time as any other API or each other..
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_MOTOR_ROTATE_MAX_STEPS
/** A hard-coded safety limit on the range of rotational movement.
 */
# define W_MOTOR_ROTATE_MAX_STEPS 600
#endif

#ifndef W_MOTOR_VERTICAL_MAX_STEPS
/** A hard-coded safety limit on the range of vertical movement.
 */
# define W_MOTOR_VERTICAL_MAX_STEPS 650
#endif

#ifndef W_MOTOR_ROTATE_DIRECTION_SENSE
/** The direction that a "1" on the rotate motor's direction
 * pin causes the motor to move: 1 since a 1 on the rotate
 * motors direction pin causes it to move towards the maximum,
 * which is W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT (otherwise it would
 * need to be -1).
 */
# define W_MOTOR_ROTATE_DIRECTION_SENSE 1
#endif

#ifndef W_MOTOR_VERTICAL_DIRECTION_SENSE
/** The direction that a "1" on the vertical motor's direction
 * pin causes the motor to move: 1 since a -1 on the vertical
 * motors direction pin causes it to move towards the maximum,
 * which is W_GPIO_PIN_INPUT_LOOK_UP_LIMIT (otherwise it would
 * need to be -1).
 */
# define W_MOTOR_VERTICAL_DIRECTION_SENSE -1
#endif

#ifndef W_MOTOR_DIRECTION_WAIT_MS
/** The pause between setting the direction that a step is to
 * take and requesting the step.
 */
# define W_MOTOR_DIRECTION_WAIT_MS 1
#endif

#ifndef W_MOTOR_STEP_WAIT_MS
/** The pause between setting a step pin output low and raising it
 * high again; also the pause between a pin being high and
 * letting it drop again.
 */
# define W_MOTOR_STEP_WAIT_MS 1
#endif

#ifndef W_MOTOR_LIMIT_MARGIN_STEPS
/** How many steps to stay clear of the limit switches in normal
 * operation.
 */
# define W_MOTOR_LIMIT_MARGIN_STEPS 10
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** The motor types; values are important as they are the index
 * to that motor in gMotor[].  The motors are calibrated in this
 * order.
 */
typedef enum {
    W_MOTOR_TYPE_VERTICAL = 0,
    W_MOTOR_TYPE_ROTATE = 1
} wMotorType_t;

/** Where the motor should sit by default, e.g. after calibration;
 * if you change the order here then you should change
 * gRestPositionStr[] to match.
 */
typedef enum {
    W_MOTOR_REST_POSITION_CENTRE,
    W_MOTOR_REST_POSITION_MAX,
    W_MOTOR_REST_POSITION_MIN
} wMotorRestPosition_t;

/** The definition of a motor.
 */
typedef struct {
    const char *name;
    unsigned int safetyLimit; // Safety limit, must be at least max - min
    int pinDisable; // The pin which when set to 1 disables the motor
    int pinDirection; // The pin which, if set to 1, makes steps * senseDirection positive
    int pinStep;  // The pin which causes the motor to step on a 0 to 1 transition
    int pinMax;   // The pin which, when pulled low, indicates max has been reached
    int pinMin;   // The pin which, when pulled low, indicates min has been reached
    int senseDirection; // 1 if a 1 at pinDirection moves towards max, else -1
    wMotorRestPosition_t restPosition;
    bool calibrated; // Ignore the remaining values if this is false
    int max;      // A calibrated limit
    int min;      // A calibrated limit
    int now;
} wMotor_t;

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise the motors: THIS WILL CAUSE MOVEMENT, calibrating
 * the movement range of the motors and, if successful, the motors
 * will be enabled on return so that wMotorMove() and wMotorMoveToRest()
 * will both function.  wGpioInit() must have returned successfully
 * before this is called.
 *
 * @return  zero on success else negative error code.
 */
int wMotorInit();

/** Try to move the given number of steps, returning the number
 * actually stepped in stepsTaken; being short on steps does not
 * constitute an error.  Will only move if calibrated unless
 * evenIfUnCalibrated is true.
 *
 * @param type               the motor type to move.
 * @param steps              the number of steps to move the motor.
 * @param stepsTaken         a pointer to a place to put the number
 *                           of steps actually taken; may be nullptr.
 * @param evenIfUnCalibrated if true then the motor will be moved
 *                           even if it is marked as uncalibrated,
 *                           else no steps will be taken if the motor
 *                           is marked as uncalibrated.
 * @return                   zero on success else negative error code.
 */
int wMotorMove(wMotorType_t type, int steps, int *stepsTaken = nullptr,
               bool evenIfUnCalibrated = false);

/** Send a motor to its rest position; will only do so if the motor
 * is calibrated.  Not being able to get to the rest position _does_
 * constitute an error.
 *
 * @param type        the motor type to move.
 * @param stepsTaken  a pointer to a place to put the number of steps
 *                    taken; may be nullptr.
 * @return            zero on success else negative error code.
 */
int wMotorMoveToRest(wMotorType_t type, int *stepsTaken = nullptr);

/** Calibrate the movement range of a motor; THIS WILL CAUSE MOVEMENT.
 * If successful then the motor will be marked as calibrated.
 *
 * @param type the motor type to calibrate.
 * @return     zero on success else negative error code.
 */
int wMotorCalibrate(wMotorType_t type);

/** Deinitialise the motors: this will disable the motors and no
 * movement will be possible until motorInit() is called once
 * more.
 */
void wMotorDeinit();

#endif // _W_MOTOR_H_

// End of file
