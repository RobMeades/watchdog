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
 * @brief The implementation of the image processing API for the
 * watchdog application.
 *
 * This code makes use of opencv, hence must be linked with opencv4.
 */

// The CPP stuff.
#include <string>
#include <memory>
#include <mutex>

// The OpenCV stuff.
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/video.hpp>

// Other parts of watchdog.
#include <w_common.h>
#include <w_util.h>
#include <w_log.h>
#include <w_msg.h>
#include <w_camera.h>

// Us.
#include <w_image_processing.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: VIEW/POINT RELATED
 * -------------------------------------------------------------- */

// View coordinates have their origin at the centre of the screen,
// as opposed to OpenCV which has its origin top left.

// Where the top of the screen lies.
#define W_VIEW_TOP ((W_CAMERA_HEIGHT_PIXELS - 1) / 2)

// Where the bottom of the screen lies.
#define W_VIEW_BOTTOM -W_VIEW_TOP

// Where the right of the screen lies.
#define W_VIEW_RIGHT ((W_CAMERA_WIDTH_PIXELS - 1) / 2)

// Where the left of the screen lies.
#define W_VIEW_LEFT -W_VIEW_RIGHT

// The view point origin in terms of an OpenCV frame.
#define W_VIEW_ORIGIN_AS_FRAME cv::Point{(W_CAMERA_WIDTH_PIXELS - 1) / 2, \
                                         (W_CAMERA_HEIGHT_PIXELS - 1) / 2}

// The OpenCV frame point origin in terms of our view coordinates.
#define W_FRAME_ORIGIN_AS_VIEW cv::Point{W_VIEW_LEFT, W_VIEW_TOP}

// The x or y-coordinate of an invalid point.
#define W_POINT_COORDINATE_INVALID -INT_MAX

// An invalid view point.
#define W_POINT_INVALID cv::Point{W_POINT_COORDINATE_INVALID, \
                                  W_POINT_COORDINATE_INVALID}

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS: DRAWING RELATED
 * -------------------------------------------------------------- */

#ifndef W_DRAWING_SHADE_WHITE
// White, to be included in an OpenCV Scalar for an 8-bit gray-scale
// image.
# define W_DRAWING_SHADE_WHITE 255
#endif

#ifndef W_DRAWING_SHADE_BLACK
// Black, to be included in an OpenCV Scalar.
# define W_DRAWING_SHADE_BLACK 0
#endif

#ifndef W_DRAWING_SHADE_LIGHT_GRAY
// Light gray, to be included in an OpenCV Scalar for an 8-bit
// gray-scale image.
# define W_DRAWING_SHADE_LIGHT_GRAY 200
#endif

#ifndef W_DRAWING_SHADE_MID_GRAY
// Mid gray, to be included in an OpenCV Scalar for an 8-bit
// gray-scale image.
# define W_DRAWING_SHADE_MID_GRAY 128
#endif

// The colour we draw around moving objects as an OpenCV Scalar.
#define W_DRAWING_SHADE_MOVING_OBJECTS cv::Scalar(W_DRAWING_SHADE_MID_GRAY, \
                                                  W_DRAWING_SHADE_MID_GRAY, \
                                                  W_DRAWING_SHADE_MID_GRAY)

// The colour we draw the focus circle in as an OpenCV Scalar.
#define W_DRAWING_SHADE_FOCUS_CIRCLE cv::Scalar(W_DRAWING_SHADE_LIGHT_GRAY, \
                                                W_DRAWING_SHADE_LIGHT_GRAY, \
                                                W_DRAWING_SHADE_LIGHT_GRAY)

#ifndef W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS
// The line thickness we use when drawing rectangles around moving
// objects: 5 is nice and chunky.
# define W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS 5
#endif

#ifndef W_DRAWING_LINE_THICKNESS_FOCUS_CIRCLE
// The line thickness for the focus circle; should be less chunky than
// the moving objects so as not to obscure anything.
# define W_DRAWING_LINE_THICKNESS_FOCUS_CIRCLE 1
#endif

#ifndef W_DRAWING_RADIUS_FOCUS_CIRCLE
// The radius of the focus circle when we draw it on the screen.
# define W_DRAWING_RADIUS_FOCUS_CIRCLE  150
#endif

// The colour we draw the date/time text.
#define W_DRAWING_DATE_TIME_TEXT_SHADE cv::Scalar(W_DRAWING_SHADE_BLACK, \
                                                  W_DRAWING_SHADE_BLACK, \
                                                  W_DRAWING_SHADE_BLACK)

#ifndef W_DRAWING_DATE_TIME_TEXT_THICKNESS
// The thickness of the line drawing the date/time text: 1 is
// perfectly readable.
# define W_DRAWING_DATE_TIME_TEXT_THICKNESS 1
#endif

#ifndef W_DRAWING_DATE_TIME_FONT_HEIGHT
// Font height scale for the date/time stamp: 0.5 is nice and small
// and fits within the rectangle set out below.
# define W_DRAWING_DATE_TIME_FONT_HEIGHT 0.5
#endif

#ifndef W_DRAWING_DATE_TIME_HEIGHT_PIXELS
// The height of the date/time box in pixels: 20 is high enough for
// text height set to 0.5 with a small margin.
# define W_DRAWING_DATE_TIME_HEIGHT_PIXELS 20
#endif

#ifndef W_DRAWING_DATE_TIME_WIDTH_PIXELS
// The width of the date/time box in pixels: 190 is wide enough for
// a full %Y-%m-%d %H:%M:%S date/time stamp with font size 0.5 and
// a small margin.
# define W_DRAWING_DATE_TIME_WIDTH_PIXELS 190
#endif

#ifndef W_DRAWING_DATE_TIME_MARGIN_PIXELS_X
// Horizontal margin for the text inside its date/time box: 2 is good.
# define W_DRAWING_DATE_TIME_MARGIN_PIXELS_X 2
#endif

#ifndef W_DRAWING_DATE_TIME_MARGIN_PIXELS_Y
// Vertical margin for the text inside its date/time box: needs to be
// more than the X margin to look right, 5 is good.
# define W_DRAWING_DATE_TIME_MARGIN_PIXELS_Y 5
#endif

// The background colour we draw the rectangle that the date/time is
// printed on.
#define W_DRAWING_DATE_TIME_REGION_SHADE cv::Scalar(W_DRAWING_SHADE_WHITE, \
                                                    W_DRAWING_SHADE_WHITE, \
                                                    W_DRAWING_SHADE_WHITE)

#ifndef W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_X
// Horizontal offset for the date/time box when it is placed into the
// main image: 5 will position it nicely on the left. 
# define W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_X 5
#endif

#ifndef W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_Y
// Vertical offset for the date/time box when it is placed into the
// main image: 5 will position it nicely at the bottom (i.e. this is
// taken away from the image height when making an OpenCV Point). 
# define W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_Y 5
#endif

#ifndef W_DRAWING_DATE_TIME_ALPHA
// Opacity, AKA alpha, of the date/time box on top of the main image,
// range 1 to 0. 0.7 is readable but you can still see the
// underlying image.
# define W_DRAWING_DATE_TIME_ALPHA 0.7
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** A point with mutex protection, used for the focus point which
 * we need to write from the control thread and read from the
 * requestCompleted() callback.  Aside from static initialisation,
 * pointProtectedSet() and pointProtectedGet() should always be
 * used to access a variable of this type.
 */
typedef struct {
    std::mutex mutex;
    cv::Point point;
} wPointProtected_t;

/** Hold information on a rectangle, likely one bounding an
 * object we think is moving.
 */
typedef struct {
    int areaPixels;
    cv::Point centreFrame; // The centre in frame coordinates
} wRectInfo_t;

/** Context needed by the image processing message handler.
 */
typedef struct {
    std::shared_ptr<cv::BackgroundSubtractor> backgroundSubtractor;
    cv::Mat maskForeground;
    wPointProtected_t focusPointView;
    wCommonFrameFunction_t *outputCallback;
    wImageProcessingFocusFunction_t *focusCallback;
} wImageProcessingContext_t;

/** Image processing message types; just the one.
 */
typedef enum {
    W_IMAGE_PROCESSING_MSG_TYPE_IMAGE_BUFFER // wImageProcessingMsgBodyImageBuffer_t
} wImageProcessingMsgType_t;

/** The message body structure corresponding to our one message:
 * W_IMAGE_PROCESSING_MSG_TYPE_IMAGE_BUFFER.
 */
typedef struct {
    uint8_t *data;
    unsigned int length;
    unsigned int sequence;
    unsigned int width;
    unsigned int height;
    unsigned int stride;
} wImageProcessingMsgBodyImageBuffer_t;

/** Union of message bodies; if you add a member here you must add a type for it in
 * wImageProcessingMsgType_t.
 */
typedef union {
    wImageProcessingMsgBodyImageBuffer_t imageBuffer;   // W_IMAGE_PROCESSING_MSG_TYPE_IMAGE_BUFFER
} wImageProcessingMsgBody_t;

/** A structure containing the message handling/freeing function
 * and the message type they handle, for use in gMsgHandler[].
 */
typedef struct {
    wImageProcessingMsgType_t msgType;
    wMsgHandlerFunction_t *function;
    wMsgHandlerFunctionFree_t *functionFree;
} wImageProcessingMsgHandler_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// NOTE: there are more messaging-related variables below
// the definition of the message handling functions.

// The ID of the image processing message queue
static int gMsgQueueId = -1;

// Image processing context.
static wImageProcessingContext_t *gContext = nullptr;

// NOTE: there are more messaging-related variables below
// the definition of the message handling functions.

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: VIEW (I.E. THE WATCHDOG'S VIEW) RELATED
 * -------------------------------------------------------------- */

// Determine whether a point is valid or not
static bool pointIsValid(const cv::Point *point)
{
    return point && (*point != W_POINT_INVALID);
}

// Convert a point from our view coordinates to those of an OpenCV
// frame, limiting at the known edges of the frame.
static int viewToFrameAndLimit(const cv::Point *pointView, cv::Point *pointFrame)
{
    int errorCode = -EINVAL;

    if (pointFrame && pointIsValid(pointView)) {
        errorCode = 0;
        // OpenCV has it is origin top-left, x increases in the same
        // direction as us, y the opposite (OpenCV increases going
        // down, we decrease going down)
        pointFrame->x = W_VIEW_ORIGIN_AS_FRAME.x + pointView->x;
        pointFrame->y = -(pointView->y - W_VIEW_ORIGIN_AS_FRAME.y);

        if (pointFrame->x < 0) {
            //W_LOG_WARN("viewToFrameAndLimit() x value of frame is"
            //           " negative (%d), limiting to zero.",
            //           pointFrame->x);
            pointFrame->x = 0;
        } else if (pointFrame->x >= W_CAMERA_WIDTH_PIXELS) {
            //W_LOG_WARN("viewToFrameAndLimit() x value of frame is"
            //           " too large (%d), limiting to %d.",
            //           pointFrame->x, W_CAMERA_WIDTH_PIXELS - 1);
            pointFrame->x = W_CAMERA_WIDTH_PIXELS - 1;
        }
        if (pointFrame->y < 0) {
            //W_LOG_WARN("viewToFrameAndLimit() y value of frame is"
            //           " negative (%d), limiting to zero.",
            //           pointFrame->y);
            pointFrame->y = 0;
        } else if (pointFrame->y >= W_CAMERA_HEIGHT_PIXELS) {
            //W_LOG_WARN("viewToFrameAndLimit() y value of frame is"
            //           " too large (%d), limiting to %d.",
            //           pointFrame->y, W_CAMERA_HEIGHT_PIXELS - 1);
            pointFrame->y = W_CAMERA_HEIGHT_PIXELS - 1;
        }
    }

    return errorCode;
}

// Convert a point from those of an OpenCV frame to our view
// coordinates, limiting at the edges of the view.
static int frameToViewAndLimit(const cv::Point *pointFrame, cv::Point *pointView)
{
    int errorCode = -EINVAL;

    if (pointView && pointIsValid(pointFrame)) {
        errorCode = 0;
       // Our origin is in the centre
        pointView->x = W_FRAME_ORIGIN_AS_VIEW.x + pointFrame->x;
        pointView->y = W_FRAME_ORIGIN_AS_VIEW.y - pointFrame->y;

        if (pointView->x < W_VIEW_LEFT) {
            //W_LOG_WARN("frameToViewAndLimit() x value of view is"
            //           " too small (%d), limiting to %d.",
            //           pointView->x, W_VIEW_LEFT);
            pointView->x = W_VIEW_LEFT;
        } else if (pointView->x > W_VIEW_RIGHT) {
            //W_LOG_WARN("frameToViewAndLimit() x value of view is"
            //           " too large (%d), limiting to %d.",
            //           pointView->x, W_VIEW_RIGHT);
            pointView->x = W_VIEW_RIGHT;
        }
        if (pointView->y < W_VIEW_BOTTOM) {
            //W_LOG_WARN("frameToViewAndLimit() y value of view is"
            //           " too small (%d), limiting to %d.",
            //           pointView->y, W_VIEW_BOTTOM);
            pointView->y = W_VIEW_BOTTOM;
        } else if (pointView->y > W_VIEW_TOP) {
            //W_LOG_WARN("frameToViewAndLimit() y value of view is"
            //           " too large (%d), limiting to %d.",
            //           pointView->y, W_VIEW_TOP);
            pointView->y = W_VIEW_TOP;
        }
    }

    return errorCode;
}

// Get the centre and area of a rectangle, limiting sensibly.
static bool rectGetInfoAndLimit(const cv::Rect *rect, wRectInfo_t *rectInfo)
{
    bool isLimited = false;

    if (rect && rectInfo) {
        rectInfo->areaPixels = rect->width * rect->height;
        rectInfo->centreFrame = {rect->x + (rect->width >> 1),
                                 rect->y + (rect->height >> 1)};
        if (rectInfo->areaPixels > W_CAMERA_AREA_PIXELS) {
            //W_LOG_WARN("rectGetInfoAndLimit() area is"
            //           " too large (%d), limiting to %d.",
            //           rectInfo->areaPixels, W_CAMERA_AREA_PIXELS);
            rectInfo->areaPixels = W_CAMERA_AREA_PIXELS;
            isLimited = true;
        }
        if (rectInfo->centreFrame.x >= W_CAMERA_WIDTH_PIXELS) {
            //W_LOG_WARN("rectGetInfoAndLimit() x frame coordinate of rectangle"
            //           " centre is too large (%d), limiting to %d.",
            //           rectInfo->centreFrame.x, W_CAMERA_WIDTH_PIXELS - 1);
            rectInfo->centreFrame.x = W_CAMERA_WIDTH_PIXELS - 1;
            isLimited = true;
        } else if (rectInfo->centreFrame.x < 0) {
            //W_LOG_WARN("rectGetInfoAndLimit() x frame coordinate of rectangle"
            //           " is negative (%d), limiting to zero.",
            //           rectInfo->centreFrame.x);
            rectInfo->centreFrame.x = 0;
            isLimited = true;
        }
        if (rectInfo->centreFrame.y >= W_CAMERA_HEIGHT_PIXELS) {
            //W_LOG_WARN("rectGetInfoAndLimit() y frame coordinate of rectangle"
            //           " centre is too large (%d), limiting to %d.",
            //           rectInfo->centreFrame.y, W_CAMERA_HEIGHT_PIXELS - 1);
            rectInfo->centreFrame.y = W_CAMERA_HEIGHT_PIXELS - 1;
            isLimited = true;
        } else if (rectInfo->centreFrame.y < 0) {
            //W_LOG_WARN("rectGetInfoAndLimit() y frame coordinate of rectangle"
            //           " is negative (%d), limiting to zero.",
            //           rectInfo->centreFrame.y);
            rectInfo->centreFrame.y = 0;
            isLimited = true;
        }
    }

    return !isLimited;
}

// Sorting function for findFocusFrame().
static bool compareRectInfo(wRectInfo_t rectInfoA,
                            wRectInfo_t rectInfoB) {
    return rectInfoA.areaPixels >= rectInfoB.areaPixels;
}

// Find where the focus should be, in frame coordinates,
// given a set of contours in frame coordinates.  The return
// value is the total area of all rectangles in pixels and,
// since rectangles can overlap, it may be large than the
// area of the frame; think of it as a kind of "magnitude
// of activity" measurement, rather than a literal area.
static int findFocusFrame(const std::vector<std::vector<cv::Point>> contours,
                          cv::Point *pointFrame)
{
    int areaPixels = 0;
    std::vector<wRectInfo_t> rectInfos;

    if (pointFrame) {
        *pointFrame = W_POINT_INVALID;
        if (!contours.empty()) {
            // Create a vector of the size and centre of the rectangles that
            // bound each contour and sort them in descending order of size
            for (auto contour: contours) {
                cv::Rect rect = boundingRect(contour);
                wRectInfo_t rectInfo;
                rectGetInfoAndLimit(&rect, &rectInfo);
                rectInfos.push_back(rectInfo);
            }
            sort(rectInfos.begin(), rectInfos.end(), compareRectInfo);

            // Go through the list; the centre of the first (therefore largest)
            // rectangle is assumed to be the centre of our focus, then each
            // rectangle after that is allowed to influence the centre in
            // proportion to its size (relative to the first one)
            cv::Point pointReferenceFrame = rectInfos.front().centreFrame;
            int areaPixelsReference = rectInfos.front().areaPixels;
            areaPixels = areaPixelsReference;
            *pointFrame = pointReferenceFrame;
            for (auto rectInfo = rectInfos.begin() + 1; rectInfo != rectInfos.end(); rectInfo++) {
                *pointFrame += ((rectInfo->centreFrame - pointReferenceFrame) * rectInfo->areaPixels) / areaPixelsReference;
                areaPixels += rectInfo->areaPixels;
            }
        }
    }

    return areaPixels;
}

// Set a variable of type wPointProtected_t.
static int pointProtectedSet(wPointProtected_t *pointProtected,
                             const cv::Point *point)
{
    int errorCode = -EINVAL;
    
    if (pointProtected) {
        pointProtected->mutex.lock();
        pointProtected->point = W_POINT_INVALID;
        if (point) {
            pointProtected->point = *point;
        }
        errorCode = 0;
        pointProtected->mutex.unlock();
    }

    return errorCode;
}

// Get a variable of type wPointProtected_t.
static cv::Point pointProtectedGet(wPointProtected_t *pointProtected)
{
    cv::Point point = W_POINT_INVALID;

    if (pointProtected) {
        pointProtected->mutex.lock();
        point = pointProtected->point;
        pointProtected->mutex.unlock();
    }

    return point;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MISC
 * -------------------------------------------------------------- */

// Release the queue, context, etc.
static void cleanUp()
{
    // Release the message queue
    if (gMsgQueueId >= 0) {
        wMsgQueueStop(gMsgQueueId);
        gMsgQueueId = -1;
    }

    if (gContext) {
        delete gContext;
        gContext = nullptr;
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wImageProcessingMsgBodyImageBuffer_t
 * -------------------------------------------------------------- */

// Message handler for wImageProcessingMsgBodyImageBuffer_t.
static void msgHandlerImageProcessingImageBuffer(void *msgBody,
                                                 unsigned int bodySize,
                                                 void *context)
{
    wImageProcessingMsgBodyImageBuffer_t *msg = &(((wImageProcessingMsgBody_t *) msgBody)->imageBuffer);
    wImageProcessingContext_t *imageProcessingContext = (wImageProcessingContext_t *) context;
    cv::Point point;

    assert(bodySize == sizeof(*msg));

    // Do the OpenCV things.  From the comment on this post:
    // https://stackoverflow.com/questions/44517828/transform-a-yuv420p-qvideoframe-into-grayscale-opencv-mat
    // ...we can bring in just the Y portion of the frame as, effectively,
    // a gray-scale image using CV_8UC1, which can be processed
    // quickly. Note that OpenCV is operating in-place on the
    // data, it does not perform a copy
    cv::Mat frameOpenCvGray(msg->height, msg->width, CV_8UC1,
                            msg->data, msg->stride);

    // Update the background model: this will cause moving areas to
    // appear as pixels with value 255, stationary areas to appear
    // as pixels with value 0
    imageProcessingContext->backgroundSubtractor->apply(frameOpenCvGray,
                                                        imageProcessingContext->maskForeground);

    // Apply thresholding to the foreground mask to remove shadows:
    // anything below the first number becomes zero, anything above
    // the first number becomes the second number
    cv::Mat maskThreshold(msg->height, msg->width, CV_8UC1);
    cv::threshold(imageProcessingContext->maskForeground, maskThreshold,
                  25, 255, cv::THRESH_BINARY);
    // Perform erosions and dilations on the mask that will remove
    // any small blobs
    cv::Mat element = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat maskDeblobbed(msg->height, msg->width, CV_8UC1);
    cv::morphologyEx(maskThreshold, maskDeblobbed, cv::MORPH_OPEN, element);

    // Find the edges of the moving areas, the ones with pixel value 255
    // in the thresholded/deblobbed mask
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(maskDeblobbed, contours, hierarchy,
                     cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    // Filter the edges to keep just the major ones
    std::vector<std::vector<cv::Point>> largeContours;
    for (auto contour: contours) {
        if (contourArea(contour) > 500) {
            largeContours.push_back(contour);
        }
    }

    // Find the place we should focus on the frame,
    // if there is one
    int areaPixels = findFocusFrame(largeContours, &point);
    if ((areaPixels > 0) && (frameToViewAndLimit(&point, &point) == 0) &&
        imageProcessingContext->focusCallback) {
        imageProcessingContext->focusCallback(point, areaPixels);
    }

#if 1
    // Draw bounding boxes
    for (auto contour: largeContours) {
        cv::Rect rect = boundingRect(contour);
        rectangle(frameOpenCvGray, rect,
                  W_DRAWING_SHADE_MOVING_OBJECTS,
                  W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS);
    }
#else
    // Draw the wiggly edges
    cv::drawContours(frameOpenCvGray, largeContours, 0,
                     W_DRAWING_SHADE_MOVING_OBJECTS,
                     W_DRAWING_LINE_THICKNESS_MOVING_OBJECTS,
                     LINE_8, hierarchy, 0);
#endif

    // Draw the current focus onto the gray OpenCV frame
    point = pointProtectedGet(&(imageProcessingContext->focusPointView));
    if (viewToFrameAndLimit(&point, &point) == 0) {
        cv::circle(frameOpenCvGray, point,
                   W_DRAWING_RADIUS_FOCUS_CIRCLE,
                   W_DRAWING_SHADE_FOCUS_CIRCLE,
                   W_DRAWING_LINE_THICKNESS_FOCUS_CIRCLE);
    }

    // Finally, write the current local time onto the frame
    // First get the time as a string
    char textBuffer[64];
    time_t rawTime;
    time(&rawTime);
    const auto localTime = localtime(&rawTime);
    // %F %T is pretty much ISO8601 format, Chinese format,
    // descending order of magnitude
    strftime(textBuffer, sizeof(textBuffer), "%F %T", localTime);
    std::string timeString(textBuffer);

    // Create a frame, filled with its shade (white), of the
    // size of the rectangle we want the time to fit inside
    cv::Mat frameDateTime(W_DRAWING_DATE_TIME_HEIGHT_PIXELS,
                          W_DRAWING_DATE_TIME_WIDTH_PIXELS, CV_8UC1,
                          W_DRAWING_DATE_TIME_REGION_SHADE);
    // Write the text to this frame in its shade (black)
    cv::putText(frameDateTime, timeString,
                cv::Point(W_DRAWING_DATE_TIME_MARGIN_PIXELS_X,
                          W_DRAWING_DATE_TIME_HEIGHT_PIXELS -
                          W_DRAWING_DATE_TIME_MARGIN_PIXELS_Y),
                cv::FONT_HERSHEY_SIMPLEX,
                W_DRAWING_DATE_TIME_FONT_HEIGHT,
                W_DRAWING_DATE_TIME_TEXT_SHADE,
                W_DRAWING_DATE_TIME_TEXT_THICKNESS);
    // Create a rectangle of the same size, positioned on the main image
    cv::Rect dateTimeRegion = cv::Rect(W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_X,
                                       msg->height - W_DRAWING_DATE_TIME_HEIGHT_PIXELS -
                                       W_DRAWING_DATE_TIME_REGION_OFFSET_PIXELS_Y,
                                       W_DRAWING_DATE_TIME_WIDTH_PIXELS,
                                       W_DRAWING_DATE_TIME_HEIGHT_PIXELS); 
    // Add frameDateTime to frameOpenCvGray inside dateTimeRegion
    cv::addWeighted(frameOpenCvGray(dateTimeRegion), W_DRAWING_DATE_TIME_ALPHA,
                    frameDateTime, 1 - W_DRAWING_DATE_TIME_ALPHA, 0.0,
                    frameOpenCvGray(dateTimeRegion));

    if (imageProcessingContext->outputCallback) {
        // Send the output to the output callback
        int queueLength = imageProcessingContext->outputCallback(msg->data,
                                                                 msg->length,
                                                                 msg->sequence,
                                                                 msg->width,
                                                                 msg->height,
                                                                 msg->stride);
        if ((wCameraFrameCountGet() % W_COMMON_FRAME_RATE_HERTZ == 0) &&
            (queueLength != wMsgQueuePreviousSizeGet(gMsgQueueId))) {
            // Print the size of the backlog once a second if it has changed
            W_LOG_DEBUG("video backlog %d frame(s).", queueLength);
            wMsgQueuePreviousSizeSet(gMsgQueueId, queueLength);
        }
    } else {
        // If there is no output callback, free the image data.
        free(msg->data);
    }
}

// Message handler free() function for wImageProcessingMsgBodyImageBuffer_t.
static void msgHandlerImageProcessingBufferFree(void *msgBody, void *context)
{
    wImageProcessingMsgBodyImageBuffer_t *msg = &(((wImageProcessingMsgBody_t *) msgBody)->imageBuffer);

    // This handler doesn't use any context
    (void) context;

    free(msg->data);
}

/* ----------------------------------------------------------------
 * MORE VARIABLES: THE MESSAGES WITH THEIR MESSAGE HANDLERS
 * -------------------------------------------------------------- */

// Array of message handlers with the message type they handle.
static wImageProcessingMsgHandler_t gMsgHandler[] = {{.msgType = W_IMAGE_PROCESSING_MSG_TYPE_IMAGE_BUFFER,
                                                      .function = msgHandlerImageProcessingImageBuffer,
                                                      .functionFree =msgHandlerImageProcessingBufferFree}};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: IMAGE PROCESSING CALLBACK
 * -------------------------------------------------------------- */

// The image processing callback that is provided to the camera API;
// populates our queue with an image buffer.
static int imageProcessingCallback(uint8_t *data, unsigned int length,
                                   unsigned int sequence,
                                   unsigned int width,
                                   unsigned int height,
                                   unsigned int stride)
{
    int queueLengthOrErrorCode = -EBADF;
    wImageProcessingMsgBodyImageBuffer_t msg = {.data = data,
                                                .length = length,
                                                .sequence = sequence,
                                                .width = width,
                                                .height = height,
                                                .stride = stride};
    if (gMsgQueueId >= 0) {
        queueLengthOrErrorCode = wMsgPush(gMsgQueueId,
                                          W_IMAGE_PROCESSING_MSG_TYPE_IMAGE_BUFFER,
                                          &msg, sizeof(msg));
        if ((wCameraFrameCountGet() % W_COMMON_FRAME_RATE_HERTZ == 0) &&
            (queueLengthOrErrorCode != wMsgQueuePreviousSizeGet(gMsgQueueId))) {
            // Print the size of the backlog once a second if it has changed
            W_LOG_DEBUG("image processing backlog %d frame(s).",
                        queueLengthOrErrorCode);
            wMsgQueuePreviousSizeSet(gMsgQueueId, queueLengthOrErrorCode);
        }
    }

    return queueLengthOrErrorCode;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise image processing.
int wImageProcessingInit()
{
    int errorCode = 0;

    if (!gContext) {
        gContext = new wImageProcessingContext_t;
        // Set the initial focus point to be invalid so that we don't
        // end up with a zero'd focus point on the image
        gContext->focusPointView.point = W_POINT_INVALID;
        errorCode = -ENOMEM;
        // Set up the OpenCV background subtractor object
        gContext->backgroundSubtractor = cv::createBackgroundSubtractorMOG2();
        if (gContext->backgroundSubtractor) {
            // Create our message queue
            errorCode = wMsgQueueStart(gContext, W_IMAGE_PROCESSING_MSG_QUEUE_MAX_SIZE, "image process");
            if (errorCode >= 0) {
                gMsgQueueId = errorCode;
                errorCode = 0;
                // Register the message handler
                for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMsgHandler)) &&
                                         (errorCode == 0); x++) {
                    wImageProcessingMsgHandler_t *handler = &(gMsgHandler[x]);
                    errorCode = wMsgQueueHandlerAdd(gMsgQueueId,
                                                    handler->msgType,
                                                    handler->function,
                                                    handler->functionFree);
                }
            }
        }
        if (errorCode != 0) {
            cleanUp();
        }
    }

    return errorCode;
}

// Become a consumer of the focus point.
int wImageProcessingFocusConsume(wImageProcessingFocusFunction_t *focusCallback)
{
    int errorCode = -EBADF;

    if (gContext) {
        gContext->focusCallback = focusCallback;
        errorCode = 0;
    }

    return errorCode;
}

// Set the focus point to be drawn on the processed image.
int wImageProcessingFocusSet(const cv::Point *pointView)
{
    int errorCode = -EBADF;

    if (gContext) {
        // Set the focus point on the image
        errorCode = pointProtectedSet(&(gContext->focusPointView), pointView);
    }

    return errorCode;
}

// Start image processing.
int wImageProcessingStart(wCommonFrameFunction_t *outputCallback)
{
    int errorCode = -EBADF;

    if (gContext) {
        gContext->outputCallback = outputCallback;
        errorCode = wCameraStart(imageProcessingCallback);
    }

    return errorCode;
}

// Stop image processing.
int wImageProcessingStop()
{
    int errorCode = -EBADF;

    if (gContext) {
        errorCode = wCameraStop();
        gContext->outputCallback = nullptr;
    }

    return errorCode;
}

// Deinitialise image processing.
void wImageProcessingDeinit()
{
    if (gContext) {
        // Stop the camera first
        wCameraStop();
        cleanUp();
    }
}

// End of file
