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
 * @brief The implementation of the message API for the watchdog application.
 */

// The CPP stuff.
#include <cstring>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <list>
#include <chrono>

// The Linux/Posix stuff.
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>

// Other parts of watchdog.
#include <w_util.h>
#include <w_log.h>

// Us.
#include <w_msg.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Container for a message type and body pair, the thing that is
 * actually queued.
 */
typedef struct {
    unsigned int type;
    void *body;
    unsigned int bodySize;
} wMsgContainer_t;

/** A message handler: the message handling function and the message
 * type it handles.
 */
typedef struct {
    unsigned int msgType;
    wMsgHandlerFunction_t *function;
    // If non-NULL then this will be called to free items _inside_
    // the message body; there is no need to call it to free the
    // message body itself, msgLoop() and queueClear()
    // will do that always.
    wMsgHandlerFunctionFree_t *functionFree;
} wMsgHandler_t;

/** Definition of a message queue.
 */
typedef struct {
    unsigned int id; // The unique ID of this message queue
    bool keepGoing; // True if this queue is in use, else false
    const char *name; // Name for queue, must not be longer than pthread_setname_np() allows (e.g. 16 characters)
    std::thread thread; // The thread at the end of the message queue
    void *context; // User context pointer for the message thread, will be passed to all message handlers
    std::list<wMsgContainer_t> containerList; // A list of messages in containers
    std::mutex mutex; // A mutex to protect the list
    unsigned int sizeMax; // The maximum number of elements that one can put in containerList
    std::list<wMsgHandler_t *> handlerList; // The list of pointers to message handlers for this queue
    unsigned int count; // The number of messages pushed, ever
    unsigned int previousSize; // The last recorded length of the list (for debug, used by the caller)
} wMsgQueue_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// A local keep-going flag.
static bool gKeepGoing = false;

// Timer that is employed by msgLoop().
static int gTimerFd = -1;

// The next message queue ID to use.
static unsigned int gQueueId = 0;

// List of message queue pointers.
static std::list<wMsgQueue_t *> gQueueList;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Return a pointer to the message queue entry with the given ID.
static wMsgQueue_t *queueGet(unsigned int queueId)
{
    wMsgQueue_t *queue = nullptr;

    for (auto x = gQueueList.begin();
         (x != gQueueList.end()) && (queue == nullptr);
         x++) {
        if ((*x)->id == queueId) {
            queue = *x;
        }
    }

    return queue;
}

// Return a pointer to the message handler entry for the given message type.
static wMsgHandler_t *handlerGet(wMsgQueue_t *queue,
                                 unsigned int msgType)
{
    wMsgHandler_t *handler = nullptr;

    if (queue) {
        for (auto x = queue->handlerList.begin();
             (x != queue->handlerList.end()) &&
             (handler == nullptr);
             x++) {
            if ((*x)->msgType == msgType) {
                handler = *x;
            }
        }
    }

    return handler;
}

// Wait for a lock on a mutex for a given time; this should only be
// used by queueClear(): msgLoop() should run off the messaging
// timer to ensure that it sleeps properly.
//
// Note: see here:
// https://stackoverflow.com/questions/44190865/stdtimed-mutextry-lock-for-fails-immediately
// ...for why one can't use try_lock_for(), which would seem like
// the more obvious approach.
static bool queueMutexTryLockFor(std::mutex *mutex, std::chrono::nanoseconds wait)
{
    bool gotLock = false;
    wUtilTimeoutStart_t startTime = wUtilTimeoutStart();

    while (mutex && !gotLock && !wUtilTimeoutExpired(startTime, wait)) {
        gotLock = mutex->try_lock();
        if (!gotLock) {
           std::this_thread::sleep_for(std::chrono::microseconds(W_MSG_QUEUE_TICK_TIMER_PERIOD_US));
        }
    }

    return gotLock;
}

// Empty a message queue.
static int queueClear(wMsgQueue_t *queue)
{
    int errorCode = -EINVAL;

    if (queue) {
        errorCode = 0;
        if (queueMutexTryLockFor(&(queue->mutex),
                                 W_MSG_QUEUE_TRY_LOCK_WAIT)) {
            while (!queue->containerList.empty()) {
                errorCode = 0;
                wMsgContainer_t msg = queue->containerList.front();
                queue->containerList.pop_front();
                // See if there is a free() function
                wMsgHandler_t *handler = handlerGet(queue, msg.type);
                if (handler && handler->functionFree) {
                    // Call the free function
                    handler->functionFree(msg.body, queue->context);
                }
                // Now free the body
                free(msg.body);
            }
            queue->mutex.unlock();
        } else {
            W_LOG_WARN("unable to lock %s message queue to clear it.",
                       queue->name);
        }
    }

    return errorCode;
}

// Stop a message queue/thread.
static void queueStop(wMsgQueue_t *queue)
{
    if (queue) {
        // Stop the thread
        queue->keepGoing = false;
        if (queue->thread.joinable()) {
            queue->thread.join();
        }
        // Clear anything left on the queue
        queueClear(queue);
        // Note: we don't remove the entry here as that
        // would invalidate pointers to list entries;
        // the list is emptied by msgDeinit().
    }
}

// Try to pop a message off a queue.  If a message is returned
// the caller MUST call free() on msg->body.
static int msgTryPop(wMsgQueue_t *queue, wMsgContainer_t *msg)
{
    int errorCode = -EINVAL;

    if (queue && msg) {
        errorCode = -EAGAIN;
        if (queue->mutex.try_lock()) {
            if (!queue->containerList.empty()) {
                *msg = queue->containerList.front();
                queue->containerList.pop_front();
                errorCode = 0;
            }
            queue->mutex.unlock();
        }
    }

    return errorCode;
}

// The message handler loop.
//
// Note: I tried a few ways of doing this:
// - the most obvious is to try_lock_for() on the mutex protecting the
//   message queue; however, try_lock_for() keeps on exiting spontanously,
//   wasting CPU cycles enormously.
// - next would be to do a try_lock() and then loop with a sleep_for() as
//   a kind of poll-interval if it fails, but that's clunky and sleep_for()
//   also has a habit of returning spontaneously.
// - experience with the GPIO stuff shows that running a timer that allows
//   us to block on a file descriptor really does sleep, so FD and poll
//   interval it is.
static void msgLoop(wMsgQueue_t *queue)
{
    uint64_t numExpiries;
    struct pollfd pollFd[1] = {0};
    struct timespec timeSpec = {.tv_sec = 1, .tv_nsec = 0};
    sigset_t sigMask;
    wMsgContainer_t msg;

    if ((gTimerFd >= 0) && queue) {

        W_LOG_DEBUG("%s: message loop has started.", queue->name);

        pollFd[0].fd = gTimerFd;
        pollFd[0].events = POLLIN | POLLERR | POLLHUP;
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGINT);
        while (queue->keepGoing && gKeepGoing && wUtilKeepGoing()) {
            // Block waiting for the messaging timer to go off for up to
            // a time, or for CTRL-C to land
            if ((ppoll(pollFd, 1, &timeSpec, &sigMask) == POLLIN) &&
                (read(gTimerFd, &numExpiries, sizeof(numExpiries)) == sizeof(numExpiries))) {
                // Pop all the messages waiting for us
                while (msgTryPop(queue, &msg) == 0) {
                    // Find the message handler for this message type
                    wMsgHandler_t *handler = handlerGet(queue, msg.type);
                    if (handler && (handler->function)) {
                        // Call the handler
                        handler->function(msg.body, msg.bodySize, queue->context);
                        queue->count++;
                    } else {
                        W_LOG_ERROR("%s: unhandled message type (%d)",
                                    queue->name, msg.type);
                    }
                    // Free the message body now that we're done
                    free(msg.body);
                }
            }
        }
    }

    W_LOG_DEBUG("%s: message loop has ended.", queue->name);
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise messaging.
int wMsgInit()
{
    int errorCode = 0;

    if (gTimerFd < 0) {
        // Set up a tick to drive msgLoop()
        errorCode = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (errorCode >= 0) {
            struct itimerspec timerSpec = {0};
            timerSpec.it_value.tv_nsec = W_MSG_QUEUE_TICK_TIMER_PERIOD_US * 1000;
            timerSpec.it_interval.tv_nsec = timerSpec.it_value.tv_nsec;
            if (timerfd_settime(errorCode, 0, &timerSpec, nullptr) == 0) {
                gTimerFd = errorCode;
                errorCode = 0;
                // Allow messaging loops to run
                gKeepGoing = true;
            } else {
                // Tidy up the timer we started on error
                close(errorCode);
                errorCode = -errno;
                W_LOG_ERROR("unable to set messaging tick timer, error code %d.",
                            errorCode);
            }
        } else {
            errorCode = -errno;
            W_LOG_ERROR("unable to create messaging tick timer, error code %d.",
                        errorCode);
        }
    }

    return errorCode;
}

// Deinitialise messaging.
void wMsgDeinit()
{
    if (gTimerFd >= 0) {
        // Close all of the message queues and their threads
        gKeepGoing = false;
        while (!gQueueList.empty()) {
            wMsgQueue_t *queue = gQueueList.front();
            // Take the queue out of the list this time
            gQueueList.pop_front();
            queueStop(queue);
            // Empty the handler list as well
            while (!queue->handlerList.empty()) {
                wMsgHandler_t *handler = queue->handlerList.front();
                queue->handlerList.pop_front();
                // Free the previously new()ed handler entry
                delete handler;
            }
            // Free the previously new()ed queue
            delete queue;
        }
        // Stop the timer
        close(gTimerFd);
        gTimerFd = -1;
    }
}

// Start a message queue/thread.
int wMsgQueueStart(void *context, unsigned int sizeMax, const char *name)
{
    int idOrErrorCode = -EBADF;
    wMsgQueue_t *queue = nullptr;

    if (gTimerFd >= 0) {
        // Create the new queue entry
        idOrErrorCode = 0;
        try {
            queue = new wMsgQueue_t;
        }
        catch (int x) {
            idOrErrorCode = -x;
            W_LOG_ERROR("unable to initialise new queue, error code %d.",
                        idOrErrorCode);
        }
        if (queue) {
            queue->id = gQueueId;
            queue->name = name;
            queue->context = context;
            queue->sizeMax = sizeMax;
            gQueueId++;
            idOrErrorCode = queue->id;
            try {
                // This will go bang if the thread cannot be created
                queue->keepGoing = true;
                queue->thread = std::thread(msgLoop, queue);
            }
            catch (int x) {
                delete queue;
                idOrErrorCode = -x;
                W_LOG_ERROR("unable to start message thread, error code %d.",
                            idOrErrorCode);
            }
            if (idOrErrorCode >= 0) {
                try {
                    // Best effort, add the name so that it is displayed when debugging
                    pthread_setname_np(queue->thread.native_handle(), queue->name);
                    // Push the message queue onto the list (this will go bang on failure)
                    gQueueList.push_back(queue);
                }
                catch (int x) {
                    // Clean up on error
                    queue->keepGoing = false;
                    if (queue->thread.joinable()) {
                        queue->thread.join();
                    }
                    delete queue;
                    idOrErrorCode = -x;
                    W_LOG_ERROR("unable to add queue to list, error code %d.",
                                idOrErrorCode);
                }
            }
        }
    }

    return idOrErrorCode;
}

// Add a message handler to a queue.
int wMsgQueueHandlerAdd(unsigned int queueId, unsigned int msgType,
                        wMsgHandlerFunction_t *function,
                        wMsgHandlerFunctionFree_t *functionFree)
{
    int errorCode = -EBADF;
    wMsgHandler_t *handler = nullptr;

    if (gTimerFd >= 0) {
        // Find the queue
        errorCode = -EINVAL;
        wMsgQueue_t *queue = queueGet(queueId);
        if (queue) {
            errorCode = 0;
            try {
                handler = new wMsgHandler_t;
            }
            catch (int x) {
                errorCode = -x;
                W_LOG_ERROR("unable to initialise new handler, error code %d.",
                            errorCode);
            }
            if (handler) {
                handler->msgType = msgType;
                handler->function = function;
                handler->functionFree = functionFree;
                queue->mutex.lock();
                try {
                    queue->handlerList.push_back(handler);
                }
                catch (int x) {
                    delete handler;
                    errorCode = -x;
                    W_LOG_ERROR("unable to add handler to list, error code %d.",
                                errorCode);
                }
                queue->mutex.unlock();
            }
        } else {
            W_LOG_ERROR("unable to find queue ID %d.", queueId);
        }
    }

    return errorCode;
}

// Stop a message queue/thread.
void wMsgQueueStop(unsigned int queueId)
{
    if (gTimerFd >= 0) {
        wMsgQueue_t *queue = queueGet(queueId);
        queueStop(queue);
    }
}

// Push a message onto a queue.  body is copied so it can be passed
// in any which way (and it is up to the caller of msgTryPop()
// to free the copied message body).
int wMsgPush(unsigned int queueId, unsigned int msgType,
             void *body, unsigned int bodySize)
{
    int queueLengthOrErrorCode = -EBADF;

    if (gTimerFd >= 0) {
        // Find the queue
        queueLengthOrErrorCode = -EINVAL;
        wMsgQueue_t *queue = queueGet(queueId);
        if (queue && queue->keepGoing) {
            queueLengthOrErrorCode = 0;
            void *bodyCopy = nullptr;
            if (bodySize > 0) {
                queueLengthOrErrorCode = -ENOMEM;
                bodyCopy = malloc(bodySize);
                if (bodyCopy) {
                    queueLengthOrErrorCode = 0;
                    memcpy(bodyCopy, body, bodySize);
                }
            }
            if (queueLengthOrErrorCode == 0) {
                wMsgContainer_t container = {.type = msgType,
                                             .body = bodyCopy,
                                             .bodySize = bodySize};
                queue->mutex.lock();
                queueLengthOrErrorCode = -ENOBUFS;
                unsigned int queueLength = queue->containerList.size();
                if (queueLength < queue->sizeMax) {
                    queueLengthOrErrorCode = queueLength + 1;
                    try {
                        queue->containerList.push_back(container);
                    }
                    catch (int x) {
                        queueLengthOrErrorCode = -x;
                    }
                }
                queue->mutex.unlock();
            }

            if (queueLengthOrErrorCode < 0) {
                if (bodyCopy) {
                    free(bodyCopy);
                }
                W_LOG_ERROR("unable to push message type %d, body length %d,"
                            " to %s message queue (%d)!",
                            msgType, bodySize, queue->name,
                            queueLengthOrErrorCode);
            }
        } else {
            W_LOG_ERROR("unable to find active queue ID %d.", queueId);
        }
    }

    return queueLengthOrErrorCode;
}

// Get the previousSize record for the given message queue, used
// by the caller for debugging queue build-ups.
int wMsgQueuePreviousSizeGet(unsigned int queueId)
{
    int previousSizeOrErrorCode = -EINVAL;

    // Find the queue
    wMsgQueue_t *queue = queueGet(queueId);
    if (queue) {
        previousSizeOrErrorCode = (int) queue->previousSize;
    }

    return previousSizeOrErrorCode;
}

// Set the previousSize record for the given message queue, used
// by the caller for debugging queue build-ups.
void wMsgQueuePreviousSizeSet(unsigned int queueId,
                              unsigned int previousSize)
{
    // Find the queue
    wMsgQueue_t *queue = queueGet(queueId);
    if (queue) {
        queue->previousSize = previousSize;
    }
}

// End of file
