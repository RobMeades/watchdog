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
 * @brief The implementation of the motor API for the watchdog application.
 */

// The CPP stuff.
#include <thread>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>
#include <w_gpio.h>

// Us.
#include <w_motor.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Movement tracking: order must match wMovementType_t.
static wMotor_t gMotor[] = {{.name = "vertical",
                             .safetyLimit = W_MOTOR_VERTICAL_MAX_STEPS,
                             .pinDisable = W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE,
                             .pinDirection = W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION,
                             .pinStep = W_GPIO_PIN_OUTPUT_VERTICAL_STEP,
                             .pinMax = W_GPIO_PIN_INPUT_LOOK_UP_LIMIT,
                             .pinMin = W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT,
                             .senseDirection = W_MOTOR_VERTICAL_DIRECTION_SENSE,
                             .restPosition = W_MOTOR_REST_POSITION_MAX,
                             .calibrated = false},
                            {.name = "rotate",
                             .safetyLimit = W_MOTOR_ROTATE_MAX_STEPS,
                             .pinDisable = W_GPIO_PIN_OUTPUT_ROTATE_DISABLE,
                             .pinDirection = W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION,
                             .pinStep = W_GPIO_PIN_OUTPUT_ROTATE_STEP,
                             .pinMax = W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT,
                             .pinMin = W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT,
                             .senseDirection = W_MOTOR_ROTATE_DIRECTION_SENSE,
                             .restPosition = W_MOTOR_REST_POSITION_CENTRE,
                             .calibrated = false}};

// Array of names for the rest positions, just for printing; must be in the
// same order as wMotorRestPosition_t.
static const char *gRestPositionStr[] = {"centre", "max", "min"};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Enable or disable motor control; a disabled motor will also be
// marked as uncalibrated since it may move freely when disabled.
static int enable(wMotor_t *motor, bool enableNotDisable = true)
{
    int errorCode = wGpioSet(motor->pinDisable, !enableNotDisable);

    if ((errorCode == 0) && !enableNotDisable) {
        // If disabling the motor, it is no longer calibrated
        motor->calibrated = false;
    }

    return errorCode;
}

// Enable or disable all motors; a disabled motor will also be
// marked as uncalibrated since it may move freely once disabled.
static int enableAll(bool enableNotDisable = true)
{
    int errorCode = 0;

    for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gMotor); x++) {
        wMotor_t *motor = &(gMotor[x]);
        int y = enable(motor, enableNotDisable);
        if (y < 0) {
            errorCode = y;
            W_LOG_ERROR("%s: error %sing motor.", motor->name,
                        enableNotDisable ? "enabl" : "disabl");
        }
    }

    return errorCode;
}

// Perform a step; will not move if at a limit; being at
// a limit does not constitute an error: supply stepTaken
// if you want to know the outcome.
static int step(wMotor_t *motor, int steps = 1, int *stepsTaken = nullptr)
{
    int errorCode = -EINVAL;

    if (stepsTaken) {
        *stepsTaken = 0;
    }

    if (motor) {
        // Check for limits
        errorCode = 0;
        if (steps > 0) {
            errorCode = wGpioGet(motor->pinMax);
        } else if (steps < 0) {
            errorCode = wGpioGet(motor->pinMin);
        }

        if (errorCode == 1) {
            // A limit level of 1 means the pin remains in its default
            // pulled-up state, we can move

            // Set the correct direction
            unsigned int levelDirection = 0;
            if (steps >= 0) {
                levelDirection = steps;
            }
            if (motor->senseDirection < 0) {
                levelDirection = !levelDirection;
            }
            errorCode = wGpioSet(motor->pinDirection, levelDirection);
            if (errorCode == 0) {
                // Wait a moment for the direction pin to settle
                std::this_thread::sleep_for(std::chrono::milliseconds(W_MOTOR_DIRECTION_WAIT_MS));
                // Send out a zero to one transition and wait
                errorCode = wGpioSet(motor->pinStep, 0);
                if (errorCode == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(W_MOTOR_STEP_WAIT_MS));
                    errorCode = wGpioSet(motor->pinStep, 1);
                    if (errorCode == 0) {
                        // Make sure we sit at a one for long enough
                        std::this_thread::sleep_for(std::chrono::milliseconds(W_MOTOR_STEP_WAIT_MS));
                    }
                }
            }
            if ((errorCode == 0) && (stepsTaken)) {
                // We have taken a step
                *stepsTaken = steps;
            }
        } else if ((errorCode == 0) && (steps != 0)) {
            W_LOG_DEBUG("%s: hit %s limit.", motor->name, steps > 0 ? "max" : "min");
        }

        if (errorCode < 0) {
            W_LOG_ERROR("%s: error %d on step.", motor->name, errorCode);
        }
    }

    return errorCode;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise the motors: THIS WILL CAUSE MOVEMENT.
int wMotorInit()
{
    int errorCode;

    W_LOG_INFO("calibrating limits of movement, STAND CLEAR!");

    // Calibrate movement
    errorCode = enableAll();
    for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMotor)) &&
                             (errorCode == 0); x++) {
        errorCode = wMotorCalibrate((wMotorType_t) x);
    }

    if (errorCode == 0) {
        W_LOG_INFO("calibration successful, moving to rest position.");
        for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMotor)) &&
                                 (errorCode == 0); x++) {
            errorCode = wMotorMoveToRest((wMotorType_t) x);
        }
    }

    if (errorCode != 0) {
        // Disable motors again if calibration or moving
        // to rest position failed
        enableAll(false);
    }

    return errorCode;
}

// Try to move the given number of steps, returning
// the number actually stepped in stepsTaken; being short
// on steps does not constitute an error.  Will only move
// if calibrated unless evenIfUnCalibrated is true.
int wMotorMove(wMotorType_t type, int steps, int *stepsTaken,
               bool evenIfUnCalibrated)
{
    int errorCode = -EINVAL;
    int stepUnit = 1;
    int stepsCompleted = 0;

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {
        wMotor_t *motor = &(gMotor[type]);
        if (motor->calibrated || evenIfUnCalibrated) {
            errorCode = 0;
            if (steps > 0) {
                if (motor->calibrated) {
                    // Limit the steps against the calibrated maximum
                    if (motor->now + steps > motor->max) {
                        steps = motor->max - motor->now;
                    }
                } else {
                    // Limit the steps against the hard-coded safety
                    if (steps > (int) motor->safetyLimit) {
                        steps = motor->safetyLimit;
                    }
                }
            } else if (steps < 0) {
                if (motor->calibrated) {
                    // Limit the steps against the calibrated minimum
                    if (motor->now + steps < motor->min) {
                        steps = motor->min - motor->now;
                    }
                } else {
                    // Limit the steps against the hard-coded safety
                    if (steps < -((int) motor->safetyLimit)) {
                        steps = -motor->safetyLimit;
                    }
                }
                stepUnit = -1;
            }

            if (motor->calibrated) {
                W_LOG_DEBUG("%s: moving %+d step(s).", motor->name, steps);
            } else {
                W_LOG_WARN("%s: uncalibrated movement of %+d step(s).",
                           motor->name, steps);
            }

            // Actually move
            int stepTaken = 1;
            for (int x = 0; (x < steps * stepUnit) && (stepTaken != 0) &&
                            (errorCode == 0); x++) {
                stepTaken = 0;
                errorCode = step(motor, stepUnit, &stepTaken);
                if (errorCode == 0) {
                    stepsCompleted += stepTaken;
                }
            }

            if (motor->calibrated) {
                motor->now += stepsCompleted;
                W_LOG_INFO("%s: now at position %d.", motor->name, motor->now);
            }

            if (stepsCompleted < steps) {
                W_LOG_WARN_START("%s: only %+d step(s) taken (%d short)",
                                 motor->name, stepsCompleted, steps - stepsCompleted);
                if (motor->calibrated) {
                    W_LOG_WARN_MORE(" motor now needs calibration");
                }
                W_LOG_WARN_MORE(".");
                W_LOG_WARN_END;
                motor->calibrated = false;
            }

            if (stepsTaken) {
                *stepsTaken = stepsCompleted;
            }
        }
    }

    return errorCode;
}

// Send a motor to its rest position; will only do so if
// the motor is calibrated.  Not being able to get to the
// rest position _does_ constitute an error.
int wMotorMoveToRest(wMotorType_t type, int *stepsTaken)
{
    int errorCode = -EINVAL;
    int steps = 0;
    int stepsCompleted = 0;

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {
        wMotor_t *motor = &(gMotor[type]);
        if (motor->calibrated) {
            errorCode = 0;
            switch (motor->restPosition) {
                case W_MOTOR_REST_POSITION_CENTRE:
                    steps = -motor->now;
                    break;
                case W_MOTOR_REST_POSITION_MAX:
                    steps = motor->max - motor->now;
                    break;
                case W_MOTOR_REST_POSITION_MIN:
                    steps = motor->min - motor->now;
                    break;
                default:
                    break;
            }

            if (steps != 0) {
                errorCode = wMotorMove(type, steps, &stepsCompleted, true);
                if (errorCode == 0) {
                    if (stepsCompleted != steps) {
                        errorCode = -ENXIO;
                        W_LOG_ERROR("%s: unable to take %+d step(s) to %s"
                                     " rest position (only %+d step(s) taken)!",
                                     motor->name, steps,
                                     gRestPositionStr[motor->restPosition],
                                     stepsCompleted);
                    }
                } else {
                    W_LOG_ERROR("%s: unable to get to rest position (error %d)!",
                                motor->name, errorCode);
                }
            }
            if (stepsTaken) {
                *stepsTaken = stepsCompleted;
            }
        }
    }

    return errorCode;
}

// Calibrate the movement range of a motor.
int wMotorCalibrate(wMotorType_t type)
{
    int errorCode = -EINVAL;
    int steps = 0;

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {
        wMotor_t *motor = &(gMotor[type]);
        errorCode = 0;
        motor->calibrated = false;
        // Move the full safety distance backwards to the min limit switch
        errorCode = wMotorMove(type, -motor->safetyLimit, &steps, true);
        if (errorCode == 0) {
            if (steps > (int) -motor->safetyLimit) {
                steps = 0;
                // Do the same in the forward direction
                errorCode = wMotorMove(type, motor->safetyLimit, &steps, true);
                if (errorCode == 0) {
                    if (steps < ((int) motor->safetyLimit)) {
                        // steps is now the distance between the minimum
                        // and maximum limits: set the current position
                        // and the limits; the margin will be just inside
                        // the limit switches so that we can move without
                        // stressing them and we know that our movement
                        // has become innaccurate if we hit them
                        steps >>= 1;
                        motor->now = steps;
                        steps -= W_MOTOR_LIMIT_MARGIN_STEPS;
                        motor->max = steps;
                        motor->min = -steps;
                        motor->calibrated = true;
                        W_LOG_INFO("%s: calibrated range +/- %d step(s).",
                                   motor->name, steps);
                    } else {
                        W_LOG_ERROR("%s: unable to calibrate, moving %+d step(s)"
                                    " from the max limit did not reach the min"
                                    " limit switch.", motor->name,
                                    motor->safetyLimit);
                    }
                }
            } else {
                W_LOG_ERROR("%s: unable to calibrate, moving %+d step(s) did"
                            " not reach the max limit switch.", motor->name,
                            motor->safetyLimit);
            }
        }

        if ((errorCode == 0) && !motor->calibrated) {
            errorCode = -ENXIO;
        }
    }

    return errorCode;
}

// Deinitialise the motors.
void wMotorDeinit()
{
    enableAll(false);
}

// End of file
