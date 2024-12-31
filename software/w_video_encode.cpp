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
 * @brief The implementation of the video encoding API for the
 * watchdog application.
 *
 * This code makes use of FFmpeg, hence must be linked with
 * the FFmpeg libraries libavformat, libavcodec, libavdevice
 * and libavutil.
 */

// The CPP stuff.
#include <string>
#include <memory>
#include <mutex>

extern "C" {
// The FFMPEG stuff, in good 'ole C.
# include <libavformat/avformat.h>
# include <libavcodec/avcodec.h>
# include <libavdevice/avdevice.h>
# include <libavutil/imgutils.h>
}

// Other parts of watchdog.
#include <w_common.h>
#include <w_util.h>
#include <w_log.h>
#include <w_msg.h>
#include <w_image_processing.h>
#include <w_hls.h>

// Us.
#include <w_video_encode.h>

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

// The stream time-base as an AVRational (integer pair, numerator
// then denominator) that FFmpeg understands.
#define W_VIDEO_ENCODE_TIME_BASE_AVRATIONAL {1, W_FRAME_RATE_HERTZ}

// The video stream frame rate in units of the video stream time-base.
#define W_VIDEO_ENCODE_FRAME_RATE_AVRATIONAL {W_FRAME_RATE_HERTZ, 1}

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Context needed by the video encode message handler.
 */
typedef struct {
    AVFormatContext *formatContext;
    AVCodecContext *codecContext;
} wVideoEncodeContext_t;

/** Video encoding message types; just the one.
 */
typedef enum {
    W_VIDEO_ENCODE_MSG_TYPE_AVFRAME_PTR_PTR // Pointer to an AVFrame * (i.e. an FFMpeg type)
} wVideoEncodeMsgType_t;

/** The message body structure corresponding to our one message:
 * W_VIDEO_ENCODE_MSG_TYPE_AVFRAME_PTR_PTR, which is actually an
 * FFmpeg AVFrame **.
 */
typedef struct AVFrame **wVideoEncodeMsgBodyAvFramePtrPtr_t;

/** Union of message bodies; if you add a member here you must add
* a type for it in wVideoEncodeMsgType_t.
 */
typedef union {
    wVideoEncodeMsgBodyAvFramePtrPtr_t avFramePtrPtr;   // W_VIDEO_ENCODE_MSG_TYPE_AVFRAME_PTR_PTR
} wVideoEncodeMsgBody_t;

/** A structure containing the message handling/freeing function
 * and the message type the handle, for use in gMsgHandler[].
 */
typedef struct {
    wVideoEncodeMsgType_t msgType;
    wMsgHandlerFunction_t *function;
    wMsgHandlerFunctionFree_t *functionFree;
} wVideoEncodeMsgHandler_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// NOTE: there are more messaging-related variables below
// the definition of the message handling functions.

// The ID of the image processing message queue
static unsigned int gMsgQueueId = -1;

// Context for video encoding.
static wVideoEncodeContext_t *gContext = nullptr;

// Storage for the video stream.
static AVStream *gAvStream = nullptr;

// Count of frames received from the video codec, purely for
// information.
static unsigned int gFrameOutputCount = 0;

// Keep track of timing on the video stream, purely for information.
static wUtilMonitorTiming_t gMonitorTiming;

// NOTE: there are more messaging-related variables below
// the definition of the message handling functions.

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Callback for FFmpeg to call when it has finished with a buffer;
// the opaque pointer should be the sequence number of the frame,
// for debug purposes.
static void avFrameFreeCallback(void *opaque, uint8_t *data)
{
    free(data);
    (void) opaque;
    // W_LOG_DEBUG("video codec is done with frame %llu.", (uint64_t) opaque);
}

// Push a frame of video data onto the queue; data _must_ be a
// malloc()ed buffer and this function _will_ free that malloc()ed
// data, even in a fail case.
static int avFrameQueuePush(uint8_t *data, unsigned int length,
                            unsigned int sequenceNumber,
                            unsigned int width, unsigned int height,
                            unsigned int yStride)
{
    int queueLengthOrErrorCode = -ENOMEM;

    AVFrame *avFrame = av_frame_alloc();
    if (avFrame) {
        avFrame->format = AV_PIX_FMT_YUV420P;
        avFrame->width = width;
        avFrame->height = height;
        // Each line size is the width of a plane (Y, U or V) plus packing,
        // e.g. in the case of a 960 pixel wide frame the stride is 1024.
        // But in YUV420 only the Y plane is at full resolution, the U and
        // V planes are at half resolution (e.g. 512), hence the divide by
        // two for planes 1 and 2 below
        avFrame->linesize[0] = yStride;
        avFrame->linesize[1] = yStride >> 1;
        avFrame->linesize[2] = yStride >> 1;
        avFrame->time_base = W_VIDEO_ENCODE_TIME_BASE_AVRATIONAL;
        avFrame->pts = sequenceNumber;
        avFrame->duration = 1;
        // avFrameFreeCallback() is the function which ultimately frees
        // the video data we are passing around, once the video codec has
        // finished with it
        avFrame->buf[0] = av_buffer_create(data, length,
                                           avFrameFreeCallback,
                                           (void *) (uint64_t) sequenceNumber,
                                           0);
        if (avFrame->buf[0]) {
            queueLengthOrErrorCode = 0;
        }
        if (queueLengthOrErrorCode == 0) {
            queueLengthOrErrorCode =  av_image_fill_pointers(avFrame->data,
                                                             AV_PIX_FMT_YUV420P,
                                                             avFrame->height,
                                                             avFrame->buf[0]->data,
                                                             avFrame->linesize);
        }
        if (queueLengthOrErrorCode >= 0) {
            queueLengthOrErrorCode = av_frame_make_writable(avFrame);
        }

        if (queueLengthOrErrorCode == 0) {
            queueLengthOrErrorCode = wMsgPush(gMsgQueueId,
                                              W_VIDEO_ENCODE_MSG_TYPE_AVFRAME_PTR_PTR,
                                              &avFrame, sizeof(avFrame));
        }

        if (queueLengthOrErrorCode < 0) {
            // This will cause avFrameFreeCallback() to be
            // called and free the data
            av_frame_free(&avFrame);
            W_LOG_ERROR("unable to push frame %d to video queue (%d)!",
                        sequenceNumber, queueLengthOrErrorCode);
        }
    }

    return queueLengthOrErrorCode;
}

// Get video from the codec and write it to the output.
static int videoOutput(AVCodecContext *codecContext, AVFormatContext *formatContext)
{
    int errorCode = -ENOMEM;
    unsigned int numReceivedPackets = 0;

    AVPacket *packet = av_packet_alloc();
    if (packet) {
        errorCode = 0;
        // Call avcodec_receive_packet until it returns AVERROR(EAGAIN),
        // meaning it needs to be fed more input
        do {
            errorCode = avcodec_receive_packet(codecContext, packet);
            if (errorCode == 0) {
                numReceivedPackets++;
                packet->time_base = W_VIDEO_ENCODE_TIME_BASE_AVRATIONAL;
                errorCode = av_interleaved_write_frame(formatContext, packet);
                // Apparently av_interleave_write_frame() unreferences the
                // packet so we don't need to worry about that
                gFrameOutputCount++;
            }
        } while (errorCode == 0);
        if ((numReceivedPackets > 0) &&
            ((errorCode == AVERROR(EAGAIN)) || (errorCode == AVERROR_EOF))) {
            // That'll do pig, that'll do
            errorCode = 0;
        } else {
            W_LOG_DEBUG("FFmpeg returned error %d (might be because it needs"
                        " more frames to form an output).", errorCode);
        }
        av_packet_free(&packet);
    } else {
         W_LOG_ERROR("unable to allocate packet for FFmpeg encode!");
    }

    return errorCode;
}

// Flush the video output.
static int videoOutputFlush(AVCodecContext *codecContext, AVFormatContext *formatContext)
{
    int errorCode = 0;

    W_LOG_DEBUG("flushing video output.");

    if (codecContext && formatContext) {
        // This puts the codec into flush mode
        errorCode = avcodec_send_frame(codecContext, nullptr);
        if (errorCode == 0) {
            errorCode = videoOutput(codecContext, formatContext);
        }
        // In case we want to use the codec again
        avcodec_flush_buffers(codecContext);
    }

    return errorCode;
}

// Release the queue, contexts, etc.
static void cleanUp()
{
    // Release the message queue
    if (gMsgQueueId >= 0) {
        wMsgQueueStop(gMsgQueueId);
        gMsgQueueId = -1;
    }

    if (gContext) {
        videoOutputFlush(gContext->codecContext,
                         gContext->formatContext);

        // Free all of the FFmpeg stuff
        if (gContext->formatContext) {
            av_write_trailer(gContext->formatContext);
        }
        avcodec_free_context(&(gContext->codecContext));
        if (gContext->formatContext) {
            avio_closep(&(gContext->formatContext->pb));
            avformat_free_context(gContext->formatContext);
        }
        delete gContext;
        gContext = nullptr;
    }
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE HANDLER wVideoEncodeMsgBodyAvFramePtrPtr_t
 * -------------------------------------------------------------- */

// Message handler for wVideoEncodeMsgBodyAvFramePtrPtr_t.
static void msgHandlerVideoEncodeAvFramePtrPtr(void *msgBody,
                                               unsigned int bodySize,
                                               void *context)
{
    wVideoEncodeMsgBodyAvFramePtrPtr_t *msg = &(((wVideoEncodeMsgBody_t *) msgBody)->avFramePtrPtr);
    AVFrame **avFrame = (AVFrame **) msg;
    wVideoEncodeContext_t *videoEncodeContext = (wVideoEncodeContext_t *) context;
    int32_t errorCode = 0;

    assert(bodySize == sizeof(*msg));

    // Procedure from https://ffmpeg.org/doxygen/7.0/group__lavc__encdec.html
    // Ownership of the data in the frame now passes
    // to the video codec and will be free'd by
    // avFrameFreeCallback()
    errorCode = avcodec_send_frame(videoEncodeContext->codecContext, *avFrame);
    if (errorCode == 0) {
        errorCode = videoOutput(videoEncodeContext->codecContext,
                                videoEncodeContext->formatContext);
        // Keep track of timing here, at the end of the 
        // complicated camera/video-frame antics, for debug
        // purposes
        wUtilMonitorTimingUpdate(&gMonitorTiming);
    } else {
        W_LOG_ERROR("error %d from avcodec_send_frame()!", errorCode);
    }
    // Now we can free the frame
    av_frame_free(avFrame);
    if ((errorCode != 0) && (errorCode != AVERROR(EAGAIN))) {
        W_LOG_ERROR("error %d from FFmpeg!", errorCode);
    }
}

// Message handler free() function for wVideoEncodeMsgBodyAvFramePtrPtr_t.
static void msgHandlerVideoEncodeAvFramePtrPtrFree(void *msgBody, void *context)
{
    wVideoEncodeMsgBodyAvFramePtrPtr_t *msg = &(((wVideoEncodeMsgBody_t *) msgBody)->avFramePtrPtr);
    AVFrame **avFrame = (AVFrame **) msg;

    // This handler doesn't use any context
    (void) context;

    av_frame_free(avFrame);
}

/* ----------------------------------------------------------------
 * MORE VARIABLES: THE MESSAGES WITH THEIR MESSAGE HANDLERS
 * -------------------------------------------------------------- */

// Array of message handlers with the message type they handle.
static wVideoEncodeMsgHandler_t gMsgHandler[] = {{.msgType = W_VIDEO_ENCODE_MSG_TYPE_AVFRAME_PTR_PTR,
                                                  .function = msgHandlerVideoEncodeAvFramePtrPtr,
                                                  .functionFree =msgHandlerVideoEncodeAvFramePtrPtrFree}};

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise video encoding.
int wVideoEncodeInit(std::string outputDirectory, std::string outputFileName)
{
    int errorCode = 0;

    if (!gContext) {
        gContext = new wVideoEncodeContext_t;
        errorCode = -ENOMEM;
        // Set up the output stream for video recording, format being
        // HLS containing H.264-encoded data.
        const AVOutputFormat *avOutputFormat = av_guess_format("hls", nullptr, nullptr);
        AVFormatContext *formatContext = nullptr;
        avformat_alloc_output_context2(&formatContext, avOutputFormat,
                                       nullptr,
                                       (outputDirectory + 
                                        std::string(W_UTIL_DIR_SEPARATOR) +
                                        outputFileName +
                                        std::string(W_HLS_PLAYLIST_FILE_EXTENSION)).c_str());
        if (formatContext) {
            gContext->formatContext = formatContext;
            // Configure the HLS options
            AVDictionary *hlsOptions = nullptr;
            // Note: the original example I was following:
            // https://medium.com/@vladakuc/hls-video-streaming-from-opencv-and-ffmpeg-828ca80b4124
            // set a load of "segment_*" (e.g. segment_time_delta='1.0', segment_list_flags='cache+live')
            // options, however, though these exist in a "stream segment muxer" (see libavformat\segment.c)
            // they don't seem to be at all associated with the HLS stream as configured here
            // and the original example was including them wrongly (it just added them with
            // an av_dict_set() individually, whereas in fact they have to be added as
            // sub-dictionary (a string of key-value pairs separated by a colon) with the
            // key "hls_segment_options") and so it wouldn't have known they had no effect
            // as avformat_write_header() ignores unused dictionary entries.  So I've
            // not included the "segment_*" options here.
            // Look at the bottom of libavformat\hlsenc.c and libavformat\mpegtsenc.c for the
            // options that _do_ apply, or pipe "ffmpeg -h full" to file and search for "HLS"
            // in the output.
            // Note: we don't apply hls_time, to set the segment size, here; instead we set
            // the "gop_size" of the codec, which is the distance between key-frames, to
            // W_HLS_SEGMENT_DURATION_SECONDS and then the HLS muxer picks that up and uses it
            // as the segment size, which is much better, since it ensures a key-frame at the
            // start of every segment.
            if ((av_dict_set(&hlsOptions, "hls_base_url",
                             std::string(W_HLS_BASE_URL W_UTIL_DIR_SEPARATOR +
                                         outputDirectory +
                                         W_UTIL_DIR_SEPARATOR).c_str(), 0) == 0) &&
                (av_dict_set(&hlsOptions, "hls_segment_type", "mpegts", 0) == 0) &&
                (av_dict_set_int(&hlsOptions, "hls_list_size", W_HLS_LIST_SIZE, 0) == 0) &&
                (av_dict_set_int(&hlsOptions, "hls_allow_cache", 0, 0) == 0) &&
                (av_dict_set(&hlsOptions, "hls_flags", "delete_segments+" // Delete segments no longer in .m3u8 file
                                                       "program_date_time", 0) == 0)) { // Not required but nice to have
                //  Set up the H264 video output stream over HLS
                gAvStream = avformat_new_stream(formatContext, nullptr);
                if (gAvStream) {
                    errorCode = -ENODEV;
                    const AVCodec *videoOutputCodec = avcodec_find_encoder_by_name("libx264");
                    if (videoOutputCodec) {
                        errorCode = -ENOMEM;
                        AVCodecContext *codecContext = avcodec_alloc_context3(videoOutputCodec);
                        if (codecContext) {
                            gContext->codecContext = codecContext;
                            W_LOG_DEBUG("video codec capabilities 0x%08x.", videoOutputCodec->capabilities);
                            codecContext->width = W_WIDTH_PIXELS;
                            codecContext->height = W_HEIGHT_PIXELS;
                            codecContext->time_base = W_VIDEO_ENCODE_TIME_BASE_AVRATIONAL;
                            codecContext->framerate = W_VIDEO_ENCODE_FRAME_RATE_AVRATIONAL;
                            // Make sure we get a key frame every segment, otherwise if the
                            // HLS client has to seek backwards from the front and can't find
                            // a key frame it may fail to play the stream
                            codecContext->gop_size = W_HLS_SEGMENT_DURATION_SECONDS * W_FRAME_RATE_HERTZ;
                            // From the discussion here:
                            // https://superuser.com/questions/908280/what-is-the-correct-way-to-fix-keyframes-in-ffmpeg-for-dash/1223359#1223359
                            // ... the intended effect of setting keyint_min to twice
                            // the GOP size is that key-frames can still be inserted
                            // at a scene-cut but they don't become the kind of key-frame
                            // that would cause a segment to end early; this keeps the rate
                            // for the HLS protocol nice and steady at W_HLS_SEGMENT_DURATION_SECONDS
                            codecContext->keyint_min = codecContext->gop_size * 2;
                            codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
                            codecContext->codec_id = AV_CODEC_ID_H264;
                            codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
                            // This is needed to include the frame duration in the encoded
                            // output, otherwise the HLS bit of av_interleaved_write_frame()
                            // will emit a warning that frames having zero duration will mean
                            // the HLS segment timing is orf
                            codecContext->flags = AV_CODEC_FLAG_FRAME_DURATION;
                            AVDictionary *codecOptions = nullptr;
                            // Note: have to set "tune" to "zerolatency" below for the hls.js HLS
                            // client to work correctly: if you do not then hls.js will only work
                            // if it is started at exactly the same time as the served stream is
                            // first started and, also, without this setting hls.js will never
                            // regain sync should it fall off the stream.  I have no idea why; it
                            // took me a week of trial and error with a zillion settings to find
                            // this out
                            if ((av_dict_set(&codecOptions, "tune", "zerolatency", 0) == 0) &&
                                (avcodec_open2(codecContext,
                                               videoOutputCodec, &codecOptions) == 0) &&
                                (avcodec_parameters_from_context(gAvStream->codecpar,
                                                                 gContext->codecContext) == 0) &&
                                (avformat_write_header(formatContext, &hlsOptions) >= 0)) {
                                // avformat_write_header() and avcodec_open2() modify
                                // the options passed to them to be any options that weren't
                                // found
                                const AVDictionaryEntry *entry = nullptr;
                                while ((entry = av_dict_iterate(hlsOptions, entry))) {
                                    W_LOG_WARN("HLS option \"%s\", or value \"%s\", not found.",
                                               entry->key, entry->value);
                                }
                                while ((entry = av_dict_iterate(codecOptions, entry))) {
                                    W_LOG_WARN("codec option \"%s\", or value \"%s\", not found.",
                                               entry->key, entry->value);
                                }
                                // Don't see why this should be necessary (everything in here
                                // seems to have its own copy of time_base: the AVCodecContext does,
                                // AVFrame does and apparently AVStream does), but the example:
                                // https://ffmpeg.org/doxygen/trunk/transcode_8c-example.html
                                // does it and if you don't do it the output has no timing.
                                gAvStream->time_base = gContext->codecContext->time_base;
                                errorCode = 0;
                            } else {
                                W_LOG_ERROR("unable to either open video codec or write AV format header!");
                            }
                        } else {
                            W_LOG_ERROR("unable to allocate memory for video output context!");
                        }
                    } else {
                        W_LOG_ERROR("unable to find H.264 codec in FFmpeg!");
                    }
                } else {
                    W_LOG_ERROR("unable to allocate memory for video output stream!");
                }
            } else {
                W_LOG_ERROR("unable to allocate memory for a dictionary entry that configures HLS!");
            }
        } else {
            W_LOG_ERROR("unable to allocate memory for video output context!");
        }

        if (errorCode == 0) {
            // Create our message queue
            errorCode = wMsgQueueStart(gContext, W_VIDEO_ENCODE_MSG_QUEUE_MAX_SIZE, "video encode");
            if (errorCode >= 0) {
                gMsgQueueId = errorCode;
                errorCode = 0;
                // Register the message handler
                for (unsigned int x = 0; (x < W_UTIL_ARRAY_COUNT(gMsgHandler)) &&
                                         (errorCode == 0); x++) {
                    wVideoEncodeMsgHandler_t *handler = &(gMsgHandler[x]);
                    errorCode = wMsgQueueHandlerAdd(gMsgQueueId,
                                                    handler->msgType,
                                                    handler->function,
                                                    handler->functionFree);
                }
            }
        }

        if (errorCode < 0) {
            cleanUp();
        }
    }

    return errorCode;
}

// Start video encoding.
int wVideoEncodeStart()
{
    int errorCode = -EBADF;

    if (gContext) {
        errorCode = wImageProcessingStart(avFrameQueuePush);
    }

    return errorCode;
}

//
int wVideoEncodeStop()
{
    int errorCode = -EBADF;

    if (gContext) {
        errorCode = wImageProcessingStop();
    }

    return errorCode;
}

// Deinitialise video encoding.
void wVideoEncodeDeinit()
{
    if (gContext) {
        wImageProcessingStop();
        cleanUp();
    }
}

// End of file
