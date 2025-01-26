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
#include <mutex>

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

// Mutex to protect the API.
static std::mutex gMutex;

// Movement tracking: order must match wMovementType_t.
// The compiler will initialise any uninitialised fields to zero.
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
                             .pinMax = W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT,
                             .pinMin = W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT,
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
// IMPORTANT: gMutex must be locked before this is called.
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
// IMPORTANT: gMutex must be locked before this is called.
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

// Set direction.
// IMPORTANT: gMutex must be locked before this is called.
static int directionSet(wMotor_t *motor, int step)
{
    int errorCode = -EINVAL;

    if (motor && (step <= 1) && (step >= -1)) {
        unsigned int levelDirection = 0;
        if (step >= 0) {
            levelDirection = step;
        }

        if (motor->senseDirection < 0) {
            levelDirection = !levelDirection;
        }

        errorCode = wGpioSet(motor->pinDirection, levelDirection);
        if (errorCode == 0) {
            motor->lastUnitStep = step;
        }
    }

    return errorCode;
}

// Perform a step; will not move if at a limit; being at
// a limit does not constitute an error: supply stepTaken
// if you want to know the outcome.
// This does NOT advance motor->now.
// IMPORTANT: gMutex must be locked before this is called.
static int stepOnce(wMotor_t *motor, int stepUnit = 1, int *stepTaken = nullptr)
{
    int errorCode = -EINVAL;

    if (stepTaken) {
        *stepTaken = 0;
    }

    if (motor) {
        // Check for limits
        errorCode = 0;
        if (stepUnit > 0) {
            errorCode = wGpioGet(motor->pinMax);
        } else if (stepUnit < 0) {
            errorCode = wGpioGet(motor->pinMin);
        }

        if (errorCode == 1) {
            // A limit level of 1 means the pin remains in its default
            // pulled-up state, we can move
            errorCode = directionSet(motor, stepUnit);
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
            if ((errorCode == 0) && (stepTaken)) {
                // We have taken a step
                *stepTaken = stepUnit;
            }
        } else if ((errorCode == 0) && (stepUnit != 0)) {
            W_LOG_DEBUG("%s: hit %s limit.", motor->name, stepUnit > 0 ? "max" : "min");
        }

        if (errorCode < 0) {
            W_LOG_ERROR("%s: error %d on step.", motor->name, errorCode);
        }
    }

    return errorCode;
}

// Take multiple steps; being short on steps does not constitue an
// error; supply stepsTaken if you want to know the outcome.
// This does NOT advance motor->now.
// IMPORTANT: gMutex must be locked before this is called.
static int stepMany(wMotor_t *motor, int steps, int *stepsTaken = nullptr)
{
    int errorCode = -EINVAL;

    if (motor) {
        errorCode = 0;
        int stepTaken = 1;
        int stepUnit = stepTaken;
        if (steps < 0) {
            stepUnit = -stepUnit;
        }
        for (int x = 0; (x < steps * stepUnit) && (stepTaken != 0) &&
                        (errorCode == 0); x++) {
            stepTaken = 0;
            errorCode = stepOnce(motor, stepUnit, &stepTaken);
            if ((errorCode == 0) && (stepsTaken)) {
                *stepsTaken += stepTaken;
            }
        }
    }

    return errorCode;
}

// Step away from a limit until the limit is no longer signalled.
// Always returns a positive number on success.
// This does NOT advance motor->now.
// IMPORTANT: gMutex must be locked before this is called.
static int stepAwayFromLimit(wMotor_t *motor)
{
    int stepsOrErrorCode = -EINVAL;
    int stepUnit = 0;
    int limitPin = -1;
    // This just for debug prints
    const char *limitStr = "none";

    if (motor) {
        // Check for max limit
        stepsOrErrorCode = wGpioGet(motor->pinMax);
        if (stepsOrErrorCode == 0) {
            // The max pin is shorted to ground, so we step negatively
            stepUnit = -1;
            limitPin = motor->pinMax;
            limitStr = "max";
        }
        if (stepsOrErrorCode >= 0) {
            // Check for min limit
            stepsOrErrorCode = wGpioGet(motor->pinMin);
            if (stepsOrErrorCode == 0) {
                // The min pin is shorted to ground
                if (stepUnit != 0) {
                    // Both pins appear to be grounded, error
                    stepsOrErrorCode = -ENXIO;
                    W_LOG_ERROR("%s: both limit switches appear to be on!",
                                motor->name);
                } else {
                    // S'OK, we can step positively
                    stepUnit = 1;
                    limitPin = motor->pinMin;
                    limitStr = "min";
                }
            } else {
                // Not an error, just not at any limit, nothing to do
                stepsOrErrorCode = 0;
            }
            if ((stepUnit != 0) && (limitPin >= 0)) {
                // Step until we are no longer at the limit pin
                int steps = 0;
                while ((stepsOrErrorCode == 0) &&
                       (steps < (int) motor->safetyLimit) &&
                       (wGpioGet(limitPin) == 0)) {
                    int stepTaken = 0;
                    stepsOrErrorCode = stepOnce(motor, stepUnit, &stepTaken);
                    if (stepTaken != stepUnit) {
                        // We need this to be an error or we could
                        // be here forever
                        stepsOrErrorCode = -ENXIO;
                    }
                    if (stepsOrErrorCode == 0) {
                        steps++;
                    }
                }
                if (wGpioGet(limitPin) == 0) {
                    // Still at the limit pin: error
                    stepsOrErrorCode = -ENXIO;
                }
                if (stepsOrErrorCode == 0) {
                    // Success
                    stepsOrErrorCode = steps;
                } else {
                    W_LOG_ERROR_START("%s: error %d moving away from %s limit",
                                       motor->name, stepsOrErrorCode, limitStr);
                    if (wGpioGet(limitPin) == 0) {
                        W_LOG_ERROR_MORE(", limit pin (%d) still in contact", limitPin);
                    }
                    W_LOG_ERROR_MORE(" after %d step(s)!", steps);
                    W_LOG_ERROR_END;
                }
                
            }
        }
    }

    return stepsOrErrorCode;
}

// Try to move the given number of steps, returning
// the number actually stepped in stepsTaken; being short
// on steps does not constitute an error.  Will only move
// if calibrated unless evenIfUnCalibrated is true.
// This advances motor->now if the motor is calibrated.
// IMPORTANT: gMutex must be locked before this is called.
static int move(wMotor_t *motor, int steps, int *stepsTaken,
                bool evenIfUnCalibrated)
{
    int errorCode = -EINVAL;
    int stepsCompleted = 0;

    if (motor) {
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
            }

            if (steps != 0) {
                // Actually move
                stepsCompleted = 0;
                errorCode = stepMany(motor, steps, &stepsCompleted);
                if (motor->calibrated) {
                    motor->now += stepsCompleted;
                }
                if (stepsCompleted < steps) {
                    W_LOG_WARN_START("%s: only %+d step(s) taken (%d short)",
                                     motor->name, stepsCompleted, steps - stepsCompleted);
                    if (motor->calibrated) {
                        W_LOG_WARN_MORE(" motor now needs calibration");
                    }
                    W_LOG_WARN_MORE(".");
                    W_LOG_WARN_END;
                    if (motor->calibrated) {
#if W_MOTOR_CALIBRATE_ONE_CALIBRATE_ALL
                        // If one motor has become uncalibrated we declare
                        // all uncalibrated
                        for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gMotor); x++) {
                            gMotor[x].calibrated = false;
                        }
#else
                        motor->calibrated = false;
#endif
                    }
                }

                if (stepsTaken) {
                    *stepsTaken = stepsCompleted;
                }
            }
        } else {
            W_LOG_WARN("%s: not calibrated, not moving.",  motor->name);
        }
    }

    return errorCode;
}

// Send a motor to its rest position; will only do so if
// the motor is calibrated.  Not being able to get to the
// rest position _does_ constitute an error.
// This uses move() and hence advances motor->now if the
// motor is calibrated.
// IMPORTANT: gMutex must be locked before this is called.
static int moveToRest(wMotor_t *motor, int *stepsTaken = nullptr)
{
    int errorCode = -EINVAL;
    int steps = 0;
    int stepsCompleted = 0;

    if (motor) {
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
                errorCode = move(motor, steps, &stepsCompleted, false);
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
        } else {
            W_LOG_WARN("%s: not calibrated, not moving to rest position.",
                       motor->name);
        }
    }

    return errorCode;
}

// Calibrate the movement range of a motor.
// IMPORTANT: gMutex must be locked before this is called.
static int calibrate(wMotor_t *motor)
{
    int errorCode = -EINVAL;
    int steps = 0;
    int throwSteps = 0;

    if (motor) {
        errorCode = 0;
        motor->calibrated = false;
        // Move the full safety distance backwards to the min limit switch
        errorCode = move(motor, -motor->safetyLimit, &steps, true);
        if (errorCode == 0) {
            if (steps > (int) -motor->safetyLimit) {
                // Now move so that we're just off the limit switch;
                // this allows for any "throw" in a gear system
                errorCode = stepAwayFromLimit(motor);
                if (errorCode >= 0) {
                    throwSteps = errorCode;
                    steps = 0;
                    // Move the full safety distance to the max limit switch
                    errorCode = move(motor, motor->safetyLimit, &steps, true);
                    if (errorCode == 0) {
                        if (steps < ((int) motor->safetyLimit)) {
                            // steps is now the distance between the minimum
                            // and maximum limits: set the current position
                            // and the limits; the margin will be just inside
                            // the limit switches so that we can move without
                            // stressing them and we know that our movement
                            // has become innaccurate if we hit them
                            // Make the range a +/- one about a central origin
                            steps >>= 1;
                            motor->now = steps;
                            steps -= W_MOTOR_LIMIT_MARGIN_STEPS;
                            motor->max = steps;
                            motor->min = -steps;
                            errorCode = 0;
                            motor->calibrated = true;
                            W_LOG_INFO_START("%s: calibrated range +/- %d step(s)",
                                             motor->name, steps);
                            if (throwSteps > 0) {
                                W_LOG_INFO_MORE(" (ignoring %d throw steps)", throwSteps);
                            }
                            W_LOG_INFO_MORE(".");
                            W_LOG_INFO_END;
                        } else {
                            W_LOG_ERROR("%s: unable to calibrate, moving %+d step(s)"
                                        " from the min limit did not reach the max"
                                        " limit switch.", motor->name,
                                        motor->safetyLimit);
                        }
                    }
                }
            } else {
                W_LOG_ERROR("%s: unable to calibrate, moving %+d step(s) did"
                            " not reach the min limit switch.", motor->name,
                            motor->safetyLimit);
            }
        }

        if ((errorCode == 0) && !motor->calibrated) {
            errorCode = -ENXIO;
        }
    }

    return errorCode;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise the motors: THIS WILL CAUSE MOVEMENT.
int wMotorInit(bool doNotOperateMotors)
{
    int errorCode = 0;

    if (!doNotOperateMotors) {

        gMutex.lock();

        W_LOG_INFO("calibrating limits of movement, STAND CLEAR!");

        // Calibrate movement
        errorCode = enableAll();
        for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMotor)) &&
                                 (errorCode == 0); x++) {
            errorCode = calibrate(&(gMotor[x]));
        }

        if (errorCode == 0) {
            W_LOG_INFO("calibration successful, moving to rest position.");
            for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMotor)) &&
                                     (errorCode == 0); x++) {
                errorCode = moveToRest(&(gMotor[x]));
            }
        }

        if (errorCode != 0) {
            // Disable motors again if calibration or moving
            // to rest position failed
            enableAll(false);
        }

        gMutex.unlock();
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

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {

        gMutex.lock();

        errorCode = move(&(gMotor[type]), steps, stepsTaken,
                         evenIfUnCalibrated);

        gMutex.unlock();
    }

    return errorCode;
}

// Send a motor to its rest position; will only do so if
// the motor is calibrated.  Not being able to get to the
// rest position _does_ constitute an error.
int wMotorMoveToRest(wMotorType_t type, int *stepsTaken)
{
    int errorCode = -EINVAL;

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {

        gMutex.lock();

        errorCode = moveToRest(&(gMotor[type]), stepsTaken);

        gMutex.unlock();
    }

    return errorCode;
}

// Determine if a motor needs calibration.
bool wMotorNeedsCalibration(wMotorType_t type)
{
    bool needsCalibration = false;

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {

        gMutex.lock();

        wMotor_t *motor = &(gMotor[type]);
        needsCalibration = !motor->calibrated;

        gMutex.unlock();
    }

    return needsCalibration;
}
// Get the descriptive name of the given motor.
std::string wMotorNameGet(wMotorType_t type)
{
    std::string name = "";

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {

        gMutex.lock();

        wMotor_t *motor = &(gMotor[type]);
        name = std::string(motor->name);

        gMutex.unlock();
    }

    return name;
}

// Calibrate the movement range of a motor.
int wMotorCalibrate(wMotorType_t type)
{
    int errorCode = -EINVAL;

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {

        gMutex.lock();

        errorCode = calibrate(&(gMotor[type]));

        gMutex.unlock();
    }

    return errorCode;
}

// Get the calibrated range of a motor.
int wMotorRangeGet(wMotorType_t type)
{
    int rangeOrErrorCode = -EINVAL;

    if (type < W_UTIL_ARRAY_COUNT(gMotor)) {

        gMutex.lock();

        wMotor_t *motor = &(gMotor[type]);
        rangeOrErrorCode = -EBADF;
        if (motor->calibrated) {
            rangeOrErrorCode = motor->max - motor->min;
        }

        gMutex.unlock();
    }

    return rangeOrErrorCode;
}

// Deinitialise the motors.
void wMotorDeinit()
{
    gMutex.lock();
    enableAll(false);
    gMutex.unlock();
}

// End of file
