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

#ifndef _W_MSG_H_
#define _W_MSG_H_

/** @file
 * @brief The messaging API for the watchdog application;
 * this API is thread-safe aside from wMsgInit(), wMsgDeinit(),
 * which should not be called at the same time as any other API or
 * each other, wMsgQueueStart() which should not be called
 * from more than one thread at the same time, and
 * wMsgQueueHandlerAdd(), which should not be called again for
 * a given queue once wMsgPush() has been called on that queue.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef W_MSG_QUEUE_TRY_LOCK_WAIT
// How long to wait for a mutex lock when pulling a message off a
// a queue (see also W_MSG_QUEUE_TICK_TIMER_PERIOD below).  This
// should be relatively long, we only need the timeout to go
// check if the loop should exit.
# define W_MSG_QUEUE_TRY_LOCK_WAIT std::chrono::seconds(1)
#endif

#ifndef W_MSG_QUEUE_TICK_TIMER_PERIOD_US
// The interval between polls for a lock on the mutex of a queue
// in microseconds.
# define W_MSG_QUEUE_TICK_TIMER_PERIOD_US 1000
#endif

#ifndef W_MSG_QUEUE_MAX_SIZE
/** The default maximum queue size.
 */
# define W_MSG_QUEUE_MAX_SIZE 100
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Function signature of a message handler function.
 *
 * @param body     a pointer to the message body, which will be
 *                 of the type registered for this handler
 *                 (see wMsgQueueHandlerAdd()).
 * @param bodySize the amount of memory pointed to by body.
 * @param context  the context pointer that was passed to
 *                 wMsgQueueStart().
 */
typedef void (wMsgHandlerFunction_t)(void *body,
                                     unsigned int bodySize,
                                     void *context);

/** Function signature of a message free function.
 *
 * @param body     a pointer to the message body, which will be
 *                 of the type registered for this handler
 *                 (see wMsgQueueHandlerAdd()).
 * @param context  the context pointer that was passed to
 *                 wMsgQueueStart().
 */
typedef void (wMsgHandlerFunctionFree_t)(void *body, void *context);

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise messaging.  If messaging is already initialised this
 * function will do nothing and return success.
 *
 * @return zero on success else negative error code.
 */
int wMsgInit();

/** Deinitialise messaging, freeing all resources.
 */
void wMsgDeinit();

/** Start a message queue/thread.  The queue will do nothing useful
 * until one or more message handlers are added.
 *
 * @param context a context pointer that will be passed to any
 *                message handler called by the queue; may be nullptr.
 * @param sizeMax the maximum number of elements that can be pushed
 *                to the message queue.
 * @param name    a name for the thread, to aid debugging only,
 *                may be nullptr; should be no more than the number
 *                of characters that pthread_setname_np() allows,
 *                e.g. 16.
 * @return        a unique ID for the message queue, which can
 *                be used with the other functions in this API,
 *                else negative error code.
 */
int wMsgQueueStart(void *context = nullptr,
                   unsigned int sizeMax = W_MSG_QUEUE_MAX_SIZE,
                   const char *name = nullptr);

/** Add a message handler to a queue.  This should only be called
* before wMsgPush() is called on the given queue, it should not be
* called again afterwards.
 *
 * @param queueId      the ID of the queue to which the message
 *                     handler is to be added.
 * @param msgType      the message type that the handler handles; should
 *                     be unique within the message queue.
 * @param function     the handler function; cannot be nullptr.
 * @param functionFree a function to free the contents of this message
 *                     type, if required; may be nullptr.  Note that there
 *                     is no need for such a function to free the overall
 *                     message body, that is done automatically, this is
 *                     only useful if there are resources within the body
 *                     that need to be free'ed in particular cases.
 * @return             zero on success, else negative error code.
 */
int wMsgQueueHandlerAdd(unsigned int queueId, unsigned int msgType,
                        wMsgHandlerFunction_t *function,
                        wMsgHandlerFunctionFree_t *functionFree = nullptr);

/** Stop a message queue/thread.  Once a message queue has been free'd
 * it cannot be used again.  You do not _have_ to call this function,
 * wMsgDeinit() will perform a full clean-up.
 *
 * @param queueId     the ID of the queue to be stopped.
 * @return            zero on success else negative error code.
 */
void wMsgQueueStop(unsigned int queueId);

/** Push a message onto a queue.
 *
 * @param queueId     the ID of the queue that the message is to
 *                    be pushed onto.
 * @param msgType     the message type that is to be pushed.
 * @param body        a pointer to the message body; may be nullptr
 *                    if msgType has no body.  The body is copied
 *                    and hence may be assembled on the stack.
 * @param bodySize    the size of body; must be 0 if body is nullptr.
 * @return            zero on success else negative error code.
 */
int wMsgPush(unsigned int queueId, unsigned int msgType,
             void *body, unsigned int bodySize);

/** Set the previousSize record for the given message queue, may
 * be useful for debugging queue build-ups.
 *
 * @param queueId      the ID of the queue.
 * @param previousSize the value to set.
 */
void wMsgQueuePreviousSizeSet(unsigned int queueId,
                              unsigned int previousSize);

/** Get the previousSize record for the given message queue, may 
 *  be useful for the caller when debugging queue build-ups.
 * 
 * @param queueId     the ID of the queue.
 * @return            on success the previousSize record, as set
 *                    by wMsgQueuePreviousSizeSet(), else negative
 *                    error code.
 */
int wMsgQueuePreviousSizeGet(unsigned int queueId);

#endif // _W_MSG_H_

// End of file
