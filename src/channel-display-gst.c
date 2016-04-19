/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015-2016 CodeWeavers, Inc

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#include "spice-client.h"
#include "spice-common.h"
#include "spice-channel-priv.h"

#include "channel-display-priv.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>


/* GStreamer decoder implementation */

typedef struct SpiceGstDecoder {
    VideoDecoder base;

    /* ---------- Video characteristics ---------- */

    int width;
    int height;
    uint32_t last_mm_time;

    /* ---------- GStreamer pipeline ---------- */

    GstAppSrc *appsrc;
    GstAppSink *appsink;
    GstElement *pipeline;
    GstClock *clock;

    /* ---------- Frame and display queues ---------- */

    GMutex display_mutex;
    GQueue *display_queue;
    GQueue *frame_meta_queue;
    guint timer_id;
} SpiceGstDecoder;


/* ---------- SpiceFrameMeta ---------- */

typedef struct _SpiceFrameMeta {
    GstClockTime timestamp;
    SpiceMsgIn *msg;
    GstSample *sample;
} SpiceFrameMeta;

static SpiceFrameMeta *create_frame_meta(GstBuffer *buffer, SpiceMsgIn *msg)
{
    SpiceFrameMeta *frame_meta = spice_new(SpiceFrameMeta, 1);
    frame_meta->timestamp = GST_BUFFER_PTS(buffer);
    frame_meta->msg = msg;
    spice_msg_in_ref(msg);
    frame_meta->sample = NULL;
    return frame_meta;
}

static void free_frame_meta(SpiceFrameMeta *frame_meta)
{
    spice_msg_in_unref(frame_meta->msg);
    if (frame_meta->sample) {
        gst_sample_unref(frame_meta->sample);
    }
    free(frame_meta);
}

static SpiceFrameMeta *pop_buffer_frame_meta(SpiceGstDecoder *decoder, GstBuffer *buffer)
{
    SpiceFrameMeta *frame_meta;
    while ((frame_meta = g_queue_pop_head(decoder->frame_meta_queue))) {
        if (frame_meta->timestamp == GST_BUFFER_PTS(buffer)) {
            return frame_meta;
        }
        /* The corresponding frame was dropped by the GStreamer pipeline
         * or the pipeline was reset while it was processing a frame.
         */
        SPICE_DEBUG("the GStreamer pipeline dropped a frame");
        spice_msg_in_unref(frame_meta->msg);
        free(frame_meta);
    }
    return NULL;
}


/* ---------- GStreamer pipeline ---------- */

static void schedule_frame(SpiceGstDecoder *decoder);

/* main context */
static gboolean display_frame(gpointer video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    decoder->timer_id = 0;

    g_mutex_lock(&decoder->display_mutex);
    SpiceFrameMeta *frame_meta = g_queue_pop_head(decoder->display_queue);
    g_mutex_unlock(&decoder->display_mutex);
    g_return_val_if_fail(frame_meta, G_SOURCE_REMOVE);

    GstBuffer *buffer = frame_meta->sample ? gst_sample_get_buffer(frame_meta->sample) : NULL;
    GstMapInfo mapinfo;
    if (!frame_meta->sample) {
        spice_warning("error: got a frame without a sample!");
    } else if (gst_buffer_map(buffer, &mapinfo, GST_MAP_READ)) {
        stream_display_frame(decoder->base.stream, frame_meta->msg, mapinfo.data);
        gst_buffer_unmap(buffer, &mapinfo);
    } else {
        spice_warning("GStreamer error: could not map the buffer");
    }
    free_frame_meta(frame_meta);

    schedule_frame(decoder);
    return G_SOURCE_REMOVE;
}

/* main loop or GStreamer streaming thread */
static void schedule_frame(SpiceGstDecoder *decoder)
{
    guint32 time = stream_get_time(decoder->base.stream);
    while (!decoder->timer_id) {
        g_mutex_lock(&decoder->display_mutex);

        SpiceFrameMeta *frame_meta = g_queue_peek_head(decoder->display_queue);
        if (!frame_meta) {
            g_mutex_unlock(&decoder->display_mutex);
            break;
        }

        SpiceStreamDataHeader *op = spice_msg_in_parsed(frame_meta->msg);
        if (time < op->multi_media_time) {
            decoder->timer_id = g_timeout_add(op->multi_media_time - time,
                                              display_frame, decoder);
        } else {
            SPICE_DEBUG("%s: rendering too late by %u ms (ts: %u, mmtime: %u), dropping ",
                        __FUNCTION__, time - op->multi_media_time,
                        op->multi_media_time, time);
            g_queue_pop_head(decoder->display_queue);
            free_frame_meta(frame_meta);
        }

        g_mutex_unlock(&decoder->display_mutex);
    }
}

/* GStreamer thread
 *
 * We cannot use GStreamer's signals because they are not always run in
 * the main context. So use a callback (lower overhead) and have it pull
 * the sample to avoid a race with free_pipeline(). This means queuing the
 * decoded frames outside GStreamer. So while we're at it, also schedule
 * the frame display ourselves in schedule_frame().
 */
GstFlowReturn new_sample(GstAppSink *gstappsink, gpointer video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    GstSample *sample = gst_app_sink_pull_sample(decoder->appsink);
    GstBuffer *buffer = sample ? gst_sample_get_buffer(sample) : NULL;
    if (sample) {
        /* Ensure the video decoder object will still be there when
         * schedule_frame() or display_frame() runs.
         */
        g_mutex_lock(&decoder->display_mutex);

        SpiceFrameMeta *frame_meta = pop_buffer_frame_meta(decoder, buffer);
        if (frame_meta) {
            frame_meta->sample = sample;
            g_queue_push_tail(decoder->display_queue, frame_meta);
        } else {
            spice_warning("error: lost a buffer meta data!");
            gst_sample_unref(sample);
        }

        g_mutex_unlock(&decoder->display_mutex);
        schedule_frame(decoder);
    } else {
        spice_warning("GStreamer error: could not pull sample");
    }
    return GST_FLOW_OK;
}

static void free_pipeline(SpiceGstDecoder *decoder)
{
    if (!decoder->pipeline) {
        return;
    }

    gst_element_set_state(decoder->pipeline, GST_STATE_NULL);
    gst_object_unref(decoder->appsrc);
    gst_object_unref(decoder->appsink);
    gst_object_unref(decoder->pipeline);
    gst_object_unref(decoder->clock);
    decoder->pipeline = NULL;
}

static gboolean create_pipeline(SpiceGstDecoder *decoder)
{
    const gchar *src_caps, *gstdec_name;
    switch (decoder->base.codec_type) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        src_caps = "caps=image/jpeg";
        gstdec_name = "jpegdec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        /* typefind is unable to identify VP8 streams by design.
         * See: https://bugzilla.gnome.org/show_bug.cgi?id=756457
         */
        src_caps = "caps=video/x-vp8";
        gstdec_name = "vp8dec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_H264:
        /* h264 streams detection works fine and setting an incomplete cap
         * causes errors. So let typefind do all the work.
         */
        src_caps = "";
        gstdec_name = "h264parse ! avdec_h264";
        break;
    default:
        spice_warning("Unknown codec type %d", decoder->base.codec_type);
        return -1;
    }

    /* - We schedule the frame display ourselves so set sync=false on appsink
     *   so the pipeline decodes them as fast as possible. This will also
     *   minimize the risk of frames getting lost when we rebuild the pipeline.
     * - Set qos=true on appsink to the elements in the GStreamer pipeline
     *   try to keep up with realtime.
     * - Set drop=false on appsink and block=true on appsrc so that when
     *   the pipeline really cannot keep up delays bubble up the pipeline
     *   all the way to queue_frame() where they may result in potentially
     *   helpful bandwidth adjustments on the Spice server.
     * - Set max-bytes=0 on appsrc so appsrc does not drop frames that may
     *   be needed by those that follow.
     */
    gchar *desc = g_strdup_printf("appsrc name=src is-live=true format=time max-bytes=0 block=true %s ! %s ! videoconvert ! appsink name=sink caps=video/x-raw,format=BGRx sync=false qos=true drop=false", src_caps, gstdec_name);
    SPICE_DEBUG("GStreamer pipeline: %s", desc);

    GError *err = NULL;
    decoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    g_free(desc);
    if (!decoder->pipeline) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    decoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "src"));
    decoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "sink"));
    GstAppSinkCallbacks appsink_cbs = {NULL, NULL, &new_sample, {NULL}};
    gst_app_sink_set_callbacks(decoder->appsink, &appsink_cbs, decoder, NULL);

    decoder->clock = gst_pipeline_get_clock(GST_PIPELINE(decoder->pipeline));

    if (gst_element_set_state(decoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        SPICE_DEBUG("GStreamer error: Unable to set the pipeline to the playing state.");
        free_pipeline(decoder);
        return FALSE;
    }

    return TRUE;
}


/* ---------- VideoDecoder's public API ---------- */

static void spice_gst_decoder_reschedule(VideoDecoder *video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;
    if (decoder->timer_id != 0) {
        g_source_remove(decoder->timer_id);
        decoder->timer_id = 0;
    }
    schedule_frame(decoder);
}

/* main context */
static void spice_gst_decoder_destroy(VideoDecoder *video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    /* Stop and free the pipeline to ensure there will not be any further
     * new_sample() call (clearing thread-safety concerns).
     */
    free_pipeline(decoder);
    g_mutex_clear(&decoder->display_mutex);

    /* Even if we kept the decoder around, once we return the stream will be
     * destroyed making it impossible to display frames. So cancel any
     * scheduled display_frame() call and drop the queued frames.
     */
    if (decoder->timer_id) {
        g_source_remove(decoder->timer_id);
    }
    SpiceFrameMeta *frame_meta;
    while ((frame_meta = g_queue_pop_head(decoder->display_queue))) {
        free_frame_meta(frame_meta);
    }
    g_queue_free(decoder->display_queue);
    while ((frame_meta = g_queue_pop_head(decoder->frame_meta_queue))) {
        free_frame_meta(frame_meta);
    }
    g_queue_free(decoder->frame_meta_queue);

    free(decoder);

    /* Don't call gst_deinit() as other parts of the client
     * may still be using GStreamer.
     */
}

static void release_buffer_data(gpointer data)
{
    SpiceMsgIn* frame_msg = (SpiceMsgIn*)data;
    spice_msg_in_unref(frame_msg);
}

static void spice_gst_decoder_queue_frame(VideoDecoder *video_decoder,
                                          SpiceMsgIn *frame_msg,
                                          int32_t latency)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    uint8_t *data;
    uint32_t size = spice_msg_in_frame_data(frame_msg, &data);
    if (size == 0) {
        SPICE_DEBUG("got an empty frame buffer!");
        return;
    }

    SpiceStreamDataHeader *frame_op = spice_msg_in_parsed(frame_msg);
    if (frame_op->multi_media_time < decoder->last_mm_time) {
        SPICE_DEBUG("new-frame-time < last-frame-time (%u < %u):"
                    " resetting stream, id %d",
                    frame_op->multi_media_time,
                    decoder->last_mm_time, frame_op->id);
        /* Let GStreamer deal with the frame anyway */
    }
    decoder->last_mm_time = frame_op->multi_media_time;

    if (latency < 0 &&
        decoder->base.codec_type == SPICE_VIDEO_CODEC_TYPE_MJPEG) {
        /* Dropping MJPEG frames has no impact on those that follow and
         * saves CPU so do it.
         */
        SPICE_DEBUG("dropping a late MJPEG frame");
        return;
    }

    int width, height;
    stream_get_dimensions(decoder->base.stream, frame_msg, &width, &height);
    if (width != decoder->width || height != decoder->height) {
        SPICE_DEBUG("video format change: width %d -> %d, height %d -> %d", decoder->width, width, decoder->height, height);
        decoder->width = width;
        decoder->height = height;
        /* TODO It would be better to flush the pipeline here */
        free_pipeline(decoder);
    }
    if (!decoder->pipeline && !create_pipeline(decoder)) {
        stream_dropped_frame(decoder->base.stream);
        return;
    }

    /* ref() the frame_msg for the buffer */
    spice_msg_in_ref(frame_msg);
    GstBuffer *buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS,
                                                    data, size, 0, size,
                                                    frame_msg, &release_buffer_data);

    GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_PTS(buffer) = gst_clock_get_time(decoder->clock) - gst_element_get_base_time(decoder->pipeline) + ((uint64_t)latency) * 1000 * 1000;

    g_mutex_lock(&decoder->display_mutex);
    g_queue_push_tail(decoder->frame_meta_queue, create_frame_meta(buffer, frame_msg));
    g_mutex_unlock(&decoder->display_mutex);

    if (gst_app_src_push_buffer(decoder->appsrc, buffer) != GST_FLOW_OK) {
        SPICE_DEBUG("GStreamer error: unable to push frame of size %d", size);
        stream_dropped_frame(decoder->base.stream);
    }
}

G_GNUC_INTERNAL
gboolean gstvideo_init(void)
{
    static int success = 0;
    if (!success) {
        GError *err = NULL;
        if (gst_init_check(NULL, NULL, &err)) {
            success = 1;
        } else {
            spice_warning("Disabling GStreamer video support: %s", err->message);
            g_clear_error(&err);
            success = -1;
        }
    }
    return success > 0;
}

G_GNUC_INTERNAL
VideoDecoder* create_gstreamer_decoder(int codec_type, display_stream *stream)
{
    SpiceGstDecoder *decoder = NULL;

    if (gstvideo_init()) {
        decoder = spice_new0(SpiceGstDecoder, 1);
        decoder->base.destroy = spice_gst_decoder_destroy;
        decoder->base.reschedule = spice_gst_decoder_reschedule;
        decoder->base.queue_frame = spice_gst_decoder_queue_frame;
        decoder->base.codec_type = codec_type;
        decoder->base.stream = stream;
        g_mutex_init(&decoder->display_mutex);
        decoder->display_queue = g_queue_new();
        decoder->frame_meta_queue = g_queue_new();
    }

    return (VideoDecoder*)decoder;
}
