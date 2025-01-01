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
 * @brief The implementation of the GPIO portion of the watchdog application.
 *
 * This code makes use of libgpiod to read/write GPIO pins, hence must
 * be linked with libgpiod.
 */

// The CPP stuff.
#include <cstring>
#include <memory>
#include <thread>
#include <atomic>

// The Linux/Posix stuff.
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <gpiod.h>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>

// Us.
#include <w_gpio.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

// Storage for debouncing a GPIO.
typedef struct {
    struct gpiod_line *line;
    unsigned int notLevelCount;
} wGpioDebounce_t;

// The possible bias for a GPIO input; if you change the order
// here then you should change gBiasStr[] to match.
typedef enum {
    W_GPIO_BIAS_NONE,
    W_GPIO_BIAS_PULL_DOWN,
    W_GPIO_BIAS_PULL_UP
} wGpioBias_t;

// A GPIO input pin, its biasing and current state.
typedef struct {
    unsigned int pin;
    const char *name;
    wGpioBias_t bias;
    unsigned int level;
    wGpioDebounce_t debounce;
} wGpioInput_t;

// The possible drive strengths for a GPIO output.
typedef enum {
    W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA = 0,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_4_MA = 1,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_6_MA = 2,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_8_MA = 3,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_10_MA = 4,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_12_MA = 5,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_14_MA = 6,
    W_GPIO_OUTPUT_DRIVE_STRENGTH_16_MA = 7
} wGpioDriveStrength_t;

// A GPIO output pin with required drive strength
// and the state the pin should be initialised to.
typedef struct {
    unsigned int pin;
    const char *name;
    wGpioDriveStrength_t driveStrength;
    unsigned int initialLevel;
} wGpioOutput_t;

// An output pin that is a PWM pin.
typedef struct {
    unsigned int pin;
    // Atomic since it is read from the PWM thread and can be
    // written by anyone
    std::atomic<unsigned int> levelPercent;
    struct gpiod_line *line;
} wGpioPwm_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// A local keep-going flag.
static bool gKeepGoing = false;

// Our GPIO chip.
static gpiod_chip *gChip = nullptr;

// The file handle of the timer that drives the GPIO read loop for
// debouncing.
static int gTimerFd = -1;

// The file handle of the timer that drives the GPIO PWM loop.
static int gPwmFd = -1;

// The handle of the GPIO read thread.
static std::thread gReadThread;

// The handle of the GPIO PWM thread.
static std::thread gPwmThread;

// Array of GPIO input pins.
static wGpioInput_t gInputPin[] = {{.pin = W_GPIO_PIN_INPUT_LOOK_LEFT_LIMIT,
                                    .name = "look left limit",
                                    .bias = W_GPIO_BIAS_PULL_UP},
                                   {.pin = W_GPIO_PIN_INPUT_LOOK_RIGHT_LIMIT,
                                    .name = "look right limit",
                                    .bias = W_GPIO_BIAS_PULL_UP},
                                   {.pin = W_GPIO_PIN_INPUT_LOOK_DOWN_LIMIT,
                                    .name = "look down limit",
                                    .bias = W_GPIO_BIAS_PULL_UP},
                                   {.pin = W_GPIO_PIN_INPUT_LOOK_UP_LIMIT,
                                    .name = "look up limit",
                                    .bias = W_GPIO_BIAS_PULL_UP}};

// Array of GPIO output pins with their drive strengths
// and initial levels.
static wGpioOutput_t gOutputPin[] = {{.pin = W_GPIO_PIN_OUTPUT_ROTATE_DISABLE,
                                      .name = "rotate disable",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                      .initialLevel = 1}, // Start disabled
                                     {.pin = W_GPIO_PIN_OUTPUT_ROTATE_DIRECTION,
                                      .name = "rotate direction",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                      .initialLevel = 0},
                                     {.pin = W_GPIO_PIN_OUTPUT_ROTATE_STEP,
                                      .name = "rotate step",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                      .initialLevel = 0},
                                     {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_DISABLE,
                                      .name = "vertical disable",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                      .initialLevel = 1}, // Start disabled
                                     {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_DIRECTION,
                                      .name = "vertical direction",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                      .initialLevel = 0},
                                     {.pin = W_GPIO_PIN_OUTPUT_VERTICAL_STEP,
                                      .name = "vertical step",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA,
                                      .initialLevel = 0},
                                     {.pin = W_GPIO_PIN_OUTPUT_EYE_LEFT,
                                      .name = "left eye",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_16_MA,
                                      .initialLevel = 0},
                                     {.pin = W_GPIO_PIN_OUTPUT_EYE_RIGHT,
                                      .name = "right eye",
                                      .driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_16_MA,
                                      .initialLevel = 0}};

// Array of PWM output pins (which must also be in gOutputPin[]).
static wGpioPwm_t gPwmPin[] = {{.pin = W_GPIO_PIN_OUTPUT_EYE_LEFT},
                               {.pin = W_GPIO_PIN_OUTPUT_EYE_RIGHT}};

// Array of names for the bias types, just for printing; must be in the same
// order as wGpioBias_t.
static const char *gBiasStr[] = {"none", "pull down", "pull up"};

// Monitor the number of times we've read GPIOs, purely for information.
static uint64_t gInputReadCount = 0;

// Monitor the start and stop time of GPIO reading, purely for information.
static std::chrono::system_clock::time_point gInputReadStart;
static std::chrono::system_clock::time_point gInputReadStop;

// Remember the number of times the GPIO read thread has not been called
// dead on time, purely for information.
static uint64_t gInputReadSlipCount = 0;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Return the line for a GPIO pin, opening the chip if necessary.
static struct gpiod_line *lineGet(unsigned int pin)
{
    struct gpiod_line *line = nullptr;

    if (!gChip) {
        gChip = gpiod_chip_open_by_number(W_GPIO_CHIP_NUMBER);
    }
    if (gChip) {
        line = gpiod_chip_get_line(gChip, pin);
    }

    return line;
}

// Release a GPIO pin if it was taken before.
static void release(struct gpiod_line *line)
{
    if (gpiod_line_consumer(line) != nullptr) {
        gpiod_line_release(line);
    }
}

// Check if a GPIO pin has already been configured as an output.
static bool isOutput(struct gpiod_line *line)
{
    return ((gpiod_line_consumer(line) != nullptr) &&
            (gpiod_line_direction(line) == GPIOD_LINE_DIRECTION_OUTPUT));
}

// Configure a GPIO pin.  level and driveStrength are ignored for
// an input pin, bias is ignored for an output pin.
static int cfg(unsigned int pin, bool isOutput,
               wGpioBias_t bias = W_GPIO_BIAS_NONE,
               unsigned int level = 0,
               wGpioDriveStrength_t driveStrength = W_GPIO_OUTPUT_DRIVE_STRENGTH_2_MA)
{
    int errorCode = -EINVAL;
    struct gpiod_line *line = lineGet(pin);

    if (line) {
        release(line);
        if (isOutput) {
            errorCode = gpiod_line_request_output(line,
                                                  W_GPIO_CONSUMER_NAME,
                                                  level);
            if (errorCode == 0) {
                // Set the drive strength; from the Raspberry Pi site:
                // https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#gpio-addresses
                // ...one writes 0x5a000000 OR'ed with the drive strength
                // in bits 0, 1 and 2 to address 0x7e10002c for GPIOs 0-27,
                // address 0x7e100030 for GPIOs 28-45 or address 0x7e100034
                // for GPIOs 46-53.  All of the GPIO pins on the Pi header
                // are in the first set, which makes things simple.
                off64_t address = 0x7e10002c;
                // Need to use mmap() to get at the memory location and that requires
                // us to find the start of the memory page our address is within 
                int pageSize = getpagesize();
                // Note: getpagesize() returns a size in bytes
                off64_t baseAddress = (address / pageSize) * pageSize;
                off64_t offset = address - baseAddress;
                //unsigned char *baseAddress = (unsigned char *) (((long int) (address + pageSize)) & (long int) ~(pageSize - 1));
                //long int offset = address - baseAddress;
                int memFd = open("/dev/mem", O_RDWR);
                if (memFd) {
                    unsigned char *addressMapped = static_cast<unsigned char *> (mmap(nullptr, offset + sizeof(int),
                                                                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                                                                      memFd, baseAddress));
                    if (addressMapped) {
                        // Since each address covers a range of pins, make sure
                        // not to lower the drive strength again if a pin in the
                        // same range is being written later
                        int value = *(addressMapped + offset);
                        if ((value & 0x07) < driveStrength) {
                            // Bit positions 3 and 4 have a slew-rate and hysteresis
                            // setting respectively; preserve those when writing
                            // the new drive strength
                            *(addressMapped + offset) = (value & 0x00000018) | 0x5a000000 | driveStrength;
                        }
                        munmap(addressMapped, offset + sizeof(int));
                    }
                    close(memFd);
                } else {
                    W_LOG_ERROR("unable to access memory: do you need sudo?");
                }
            }
        } else {
            int flags = 0;
            switch (bias) {
                case W_GPIO_BIAS_PULL_DOWN:
                    flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;
                    break;
                case W_GPIO_BIAS_PULL_UP:
                    flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
                    break;
                default:
                    break;
            }
            errorCode = gpiod_line_request_input_flags(line,
                                                       W_GPIO_CONSUMER_NAME,
                                                       flags);
        }
    }

    return errorCode;
}

// Get the state of a GPIO pin without any debouncing.
static int rawGet(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;

    struct gpiod_line *line = lineGet(pin);
    if (line) {
        levelOrErrorCode = gpiod_line_get_value(line);
    }

    return levelOrErrorCode;
}

// GPIO task/thread/thing to debounce inputs and provide a stable
// input level in gInputPin[].
//
// Note: it would have been nice to read the GPIOs in a signal handler
// directly but the libgpiod functions are not async-safe (brgl,
// author of libgpiod, confirmed this), hence we use a timer and read
// the GPIOs in this thread, triggered from that timer.  This loop
// should be run at max priority; any timer ticks that are missed are
// monitored in gInputReadSlipCount.
static void readLoop(int timerFd)
{
    unsigned int x = 0;
    unsigned int level;
    uint64_t numExpiriesSaved = 0;
    uint64_t numExpiries;
    uint64_t numExpiriesPassed;
    struct pollfd pollFd[1] = {0};
    struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
    sigset_t sigMask;

    pollFd[0].fd = timerFd;
    pollFd[0].events = POLLIN | POLLERR | POLLHUP;
    sigemptyset(&sigMask);
    sigaddset(&sigMask, SIGINT);
    gInputReadStart = std::chrono::system_clock::now();
    W_LOG_DEBUG("GPIO read loop has started");
    while (gKeepGoing && wUtilKeepGoing()) {
        // Block waiting for the tick timer to go off for up to a time,
        // or for CTRL-C to land; when the timer goes off the number
        // of times it has expired is returned in numExpiries
        if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
            (read(timerFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {
            gInputReadCount++;
            numExpiriesPassed = numExpiries - numExpiriesSaved;
            if (numExpiriesPassed > 1) {
                gInputReadSlipCount += numExpiriesPassed - 1;
            }
            // Read the level from the next input pin in the array
            wGpioInput_t *gpioInput = &(gInputPin[x]);
            level = gpiod_line_get_value(gpioInput->debounce.line);
            if (gpioInput->level != level) {
                // Level is different to the last stable level, increment count
                gpioInput->debounce.notLevelCount++;
                if (gpioInput->debounce.notLevelCount > W_GPIO_DEBOUNCE_THRESHOLD) {
                    // Count is big enough that we're sure, set the level and
                    // reset the count
                    gpioInput->level = level;
                    gpioInput->debounce.notLevelCount = 0;
                }
            } else {
                // Current level is the same as the stable level, zero the change count
                gpioInput->debounce.notLevelCount = 0;
            }

            // Next input pin next time
            x++;
            if (x >= W_UTIL_ARRAY_COUNT(gInputPin)) {
                x = 0;
            }
            numExpiriesSaved = numExpiries;
        }
    }
    gInputReadStop = std::chrono::system_clock::now();

    W_LOG_DEBUG("GPIO read loop has exited");
}

// Task/thread/thing to drive the PWM output of the pins in gPwmPin[].
static void pwmLoop(int pwmFd)
{
    unsigned int pwmCount = 0;
    uint64_t numExpiries;
    struct pollfd pollFd[1] = {0};
    struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
    sigset_t sigMask;
    wGpioPwm_t *gpioPwmPinCopy = (wGpioPwm_t *) malloc(sizeof(gPwmPin));

    if (gpioPwmPinCopy) {
        pollFd[0].fd = pwmFd;
        pollFd[0].events = POLLIN | POLLERR | POLLHUP;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);
        W_LOG_DEBUG("GPIO PWM loop has started");
        while (gKeepGoing && wUtilKeepGoing()) {
            // Block waiting for the PWM timer to go off for up to a time,
            // or for CTRL-C to land
            // Change the level of a PWM pin only at the end of a PWM period
            // to avoid any chance of flicker
            memcpy((void *) gpioPwmPinCopy, gPwmPin, sizeof(gPwmPin));
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(pwmFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {
                // Progress all of the PWM pins
                wGpioPwm_t *gpioPwm = gpioPwmPinCopy;
                for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gPwmPin); x++) {
                    // If the "percentage" count has passed beyond the value
                    // for this pin, set the output low, otherwise if we're
                    // starting the count again and the percentage is non-zero,
                    // set the output pin high
                    if (pwmCount == 0) {
                        if (gpioPwm->levelPercent > 0) {
                            gpiod_line_set_value(gpioPwm->line, 1);
                        }
                    } else if (pwmCount >= gpioPwm->levelPercent * W_GPIO_PWM_MAX_COUNT / 100) {
                        gpiod_line_set_value(gpioPwm->line, 0);
                    }
                    gpioPwm++;
                }
                pwmCount++;
                if (pwmCount >= W_GPIO_PWM_MAX_COUNT) {
                    pwmCount = 0;
                    gpioPwm = gpioPwmPinCopy;
                    // Take a new copy of the pin levels
                    memcpy((void *) gpioPwmPinCopy, gPwmPin, sizeof(gPwmPin));
                }
            }
        }

        // Free memory
        free(gpioPwmPinCopy);
    } else {
        W_LOG_ERROR("unable to allocate %d byte(s) for gpioPwmPin!");
    }

    W_LOG_DEBUG("GPIO PWM loop has exited.");
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise the GPIO pins.
int wGpioInit()
{
    int errorCode = 0;

    if ((gTimerFd < 0) && (gPwmFd < 0)) {
        gKeepGoing = true;
        // Configure all of the input pins and get their initial states
        for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gInputPin)) &&
                                 (errorCode == 0); x++) {
            wGpioInput_t *gpioInput = &(gInputPin[x]);
            errorCode = cfg(gpioInput->pin, false, gpioInput->bias);
            if (errorCode == 0) {
                gpioInput->level = rawGet(gpioInput->pin);
                gpioInput->debounce.line = lineGet(gpioInput->pin);
                gpioInput->debounce.notLevelCount = 0;
            } else {
                W_LOG_ERROR("unable to set pin %d as an input with bias %s!",
                            gpioInput->pin,
                            gBiasStr[gpioInput->bias]);
            }
        }

        // Configure all of the output pins to their initial states
        for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gOutputPin)) &&
                                 (errorCode == 0); x++) {
            wGpioOutput_t *gpioOutput = &(gOutputPin[x]);
            errorCode = cfg(gpioOutput->pin, true, W_GPIO_BIAS_NONE,
                            gpioOutput->initialLevel,
                            gpioOutput->driveStrength);
            if (errorCode != 0) {
                W_LOG_ERROR("unable to set pin %d as an output,"
                            " drive strength %d and %s!",
                            gpioOutput->pin,
                            gpioOutput->driveStrength,
                            gpioOutput->initialLevel ? "high" : "low");
            }
        }

        if (errorCode == 0) {
            // Set up a tick to drive gpioInputLoop()
            errorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (errorCode >= 0) {
                struct itimerspec timerSpec = {0};
                timerSpec.it_value.tv_nsec = W_GPIO_TICK_TIMER_PERIOD_US * 1000;
                timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
                if (timerfd_settime(errorCode, 0, &timerSpec, nullptr) == 0) {
                    gTimerFd = errorCode;
                    errorCode = 0;
                } else {
                    // Tidy up the read loop timer we opened on error
                    close(errorCode);
                    errorCode = -errno;
                    W_LOG_ERROR("unable to set GPIO read timer, error code %d.",
                                errorCode);
                }
            } else {
                errorCode = -errno;
                W_LOG_ERROR("unable to create GPIO read timer, error code %d.",
                            errorCode);
            }
        }

        if (errorCode == 0) {
            // Set up a tick to drive PWM
            errorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (errorCode >= 0) {
                struct itimerspec timerSpec = {0};
                timerSpec.it_value.tv_nsec = W_GPIO_PWM_TIMER_PERIOD_US * 1000;
                timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
                if (timerfd_settime(errorCode, 0, &timerSpec, nullptr) == 0) {
                    gPwmFd = errorCode;
                    errorCode = 0;
                    // Now that the timer is set, populate gPwmPin
                    for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gPwmPin); x++) {
                        wGpioPwm_t *gpioPwm = &(gPwmPin[x]);
                        gpioPwm->line = lineGet(gpioPwm->pin);
                        gpioPwm->levelPercent = 0;
                        for (unsigned int y = 0; y < W_UTIL_ARRAY_COUNT(gOutputPin); y++) {
                            wGpioOutput_t *gpioOutput = &(gOutputPin[y]);
                            if (gpioPwm->pin == gpioOutput->pin) {
                                gpioPwm->levelPercent = gpioOutput->initialLevel * 100;
                                break;
                            }
                        }
                    }
                } else {
                    // Tidy up the PWM loop timer we opened, and the read loop timer,
                    // on error
                    close(errorCode);
                    close(gTimerFd);
                    gTimerFd = -1;
                    errorCode = -errno;
                    W_LOG_ERROR("unable to set GPIO PWM timer, error code %d.",
                                errorCode);
                }
            } else {
                // Tidy up the read loop timer on error
                close(gTimerFd);
                gTimerFd = -1;
                errorCode = -errno;
                W_LOG_ERROR("unable to create PWM timer, error code %d.", errorCode);
            }
        }
    }

    if (errorCode == 0) {
        // Start the PWM thread
        gPwmThread = std::thread(pwmLoop, gPwmFd);
        pthread_setname_np(gPwmThread.native_handle(), "pwmLoop");
        // Start the read thread
        gReadThread = std::thread(readLoop, gTimerFd);
        pthread_setname_np(gReadThread.native_handle(), "readLoop");
        // Set the thread priority for GPIO reads
        // to maximum as we never want to miss one
        struct sched_param scheduling;
        scheduling.sched_priority = sched_get_priority_max(SCHED_FIFO);
        errorCode = pthread_setschedparam(gReadThread.native_handle(),
                                          SCHED_FIFO, &scheduling);
        if (errorCode != 0) {
            // Tidy up everything on error
            wGpioDeinit();
            W_LOG_ERROR("unable to set thread priority for GPIO reads"
                        " (error %d), maybe need sudo?", errorCode);
        }
    }

    return errorCode;
}

// Get the state of a GPIO pin after debouncing.
int wGpioGet(unsigned int pin)
{
    int levelOrErrorCode = -EINVAL;

    for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gInputPin)) &&
                             (levelOrErrorCode < 0); x++) {
        wGpioInput_t *gpioInput = &(gInputPin[x]);
        if (gpioInput->pin == pin) {
            levelOrErrorCode = gpioInput->level;
        }
    }

    return levelOrErrorCode;
}

// Set the state of a GPIO pin.
int wGpioSet(unsigned int pin, unsigned int level)
{
    int errorCode = -EINVAL;
    struct gpiod_line *line = lineGet(pin);

    if (line) {
        if (isOutput(line)) {
            errorCode = gpiod_line_set_value(line, level);
        } else {
            release(line);
            errorCode = gpiod_line_request_output(line,
                                                  W_GPIO_CONSUMER_NAME,
                                                  level);
        }
    }

    return errorCode;
}

// Set the state of a GPIO PWM pin.
int wGpioPwmSet(unsigned int pin, unsigned int levelPercent)
{
    int errorCode = -EINVAL;

    for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gPwmPin)) &&
                             (errorCode < 0); x++) {
        wGpioPwm_t *gpioPwm = &(gPwmPin[x]);
        if (gpioPwm->pin == pin) {
            gpioPwm->levelPercent = levelPercent;
            errorCode = 0;
        }
    }

    return errorCode;
}

// Deinitialise the GPIO pins.
void wGpioDeinit()
{
    struct gpiod_line *line;

    if (gKeepGoing) {
        // If we have run, print some diagnostic info
        uint64_t gpioReadsPerInput = gInputReadCount / W_UTIL_ARRAY_COUNT(gInputPin);
        W_LOG_INFO_START("each GPIO input read (and debounced) every %lld ms",
                         (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds> (gInputReadStop - gInputReadStart).count() *
                         W_GPIO_DEBOUNCE_THRESHOLD / gpioReadsPerInput);
        if (gInputReadSlipCount > 0) {
            W_LOG_INFO_MORE(", GPIO input read thread was not called on schedule %lld time(s).",
                            gInputReadSlipCount);
        }
        W_LOG_INFO_MORE(".");
        W_LOG_INFO_END;
    }

    // No longer running
    gKeepGoing = false;

    // Stop the GPIO read timer
    if (gTimerFd >= 0) {
        if (gReadThread.joinable()) {
           gReadThread.join();
        }
        close(gTimerFd);
        gTimerFd = -1;
    }
    // Stop the GPIO PWM timer
    if (gPwmFd >= 0) {
        if (gPwmThread.joinable()) {
           gPwmThread.join();
        }
        close(gPwmFd);
        gPwmFd = -1;
    }

    for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gInputPin); x++) {
        line = lineGet(x);
        if (line) {
            release(line);
        }
    }
    for (unsigned int x = 0; x < W_UTIL_ARRAY_COUNT(gOutputPin); x++) {
        line = lineGet(x);
        if (line) {
            release(line);
        }
    }
}

// End of file
