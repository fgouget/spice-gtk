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

    /* ---------- Lifetime management ---------- */

    gint refs;

    /* ---------- Video characteristics ---------- */

    int width;
    int height;
    uint32_t last_mm_time;

    /* ---------- GStreamer pipeline ---------- */

    GstAppSrc *appsrc;
    GstAppSink *appsink;
    GstElement *pipeline;
    GstClock *clock;

    /* ---------- Display queue ---------- */

    GMutex display_mutex;
    GQueue *display_queue;
    guint timer_id;
} SpiceGstDecoder;


/* ---------- SpiceFrameMeta ---------- */

/* Lets us attach Spice frame data to GStreamer buffers */
typedef struct _SpiceFrameMeta {
    GstMeta meta;
    SpiceMsgIn *msg;
} SpiceFrameMeta;

static GType spice_frame_meta_api_get_type(void)
{
  static volatile GType type;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter(&type)) {
      GType _type = gst_meta_api_type_register("SpiceFrameMetaAPI", tags);
      g_once_init_leave(&type, _type);
  }
  return type;
}
#define SPICE_FRAME_META_API_TYPE (spice_frame_meta_api_get_type())

#define SPICE_FRAME_META_INFO (spice_frame_meta_get_info())
static const GstMetaInfo *spice_frame_meta_get_info(void);

#define gst_buffer_get_spice_frame_meta(b) \
  ((SpiceFrameMeta*)gst_buffer_get_meta((b), SPICE_FRAME_META_API_TYPE))

static gboolean spice_frame_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer)
{
  SpiceFrameMeta *frame_meta = (SpiceFrameMeta*)meta;

  frame_meta->msg = NULL;
  return TRUE;
}

static SpiceFrameMeta *gst_buffer_add_spice_frame_meta(GstBuffer *buffer, SpiceMsgIn *frame_msg)
{
  g_return_val_if_fail(GST_IS_BUFFER (buffer), NULL);

  SpiceFrameMeta *frame_meta = (SpiceFrameMeta*)gst_buffer_add_meta(buffer, SPICE_FRAME_META_INFO, NULL);

  spice_msg_in_ref(frame_msg);
  frame_meta->msg = frame_msg;
  return frame_meta;
}

static gboolean spice_frame_meta_transform(GstBuffer *transbuf, GstMeta *meta,
                                           GstBuffer *buffer, GQuark type,
                                           gpointer data)
{
  SpiceFrameMeta *frame_meta = (SpiceFrameMeta*)meta;

  int refcount = GST_OBJECT_REFCOUNT(transbuf);
  if (refcount > 1) {
      /* See https://bugzilla.gnome.org/show_bug.cgi?id=757254 */
      if (refcount == 2) {
          gst_buffer_unref(transbuf);
      }
  }
  if (gst_buffer_is_writable(transbuf)) {
      /* Just preserve the Spice frame information as is */
      gst_buffer_add_spice_frame_meta(transbuf, frame_meta->msg);
  }
  if (refcount == 2) {
      gst_buffer_ref(transbuf);
  }
  return TRUE;
}

static void spice_frame_meta_free(GstMeta *meta, GstBuffer *buffer)
{
  SpiceFrameMeta *frame_meta = (SpiceFrameMeta*)meta;

  spice_msg_in_unref(frame_meta->msg);
  frame_meta->msg = NULL;
}

static const GstMetaInfo *spice_frame_meta_get_info(void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter(&meta_info)) {
    const GstMetaInfo *mi = gst_meta_register(SPICE_FRAME_META_API_TYPE,
        "SpiceFrameMeta",
        sizeof(SpiceFrameMeta),
        spice_frame_meta_init,
        spice_frame_meta_free,
        spice_frame_meta_transform);
    g_once_init_leave(&meta_info, mi);
  }
  return meta_info;
}


/* ---------- SpiceGstDecoder lifetime management ---------- */

static void decoder_ref(SpiceGstDecoder *decoder)
{
    g_atomic_int_inc(&decoder->refs);
}

static void decoder_unref(SpiceGstDecoder *decoder)
{
    if (g_atomic_int_dec_and_test(&decoder->refs)) {
        /* At this point spice_gst_decoder_destroy() has already
         * released all the resources.
         */
        g_free(decoder);
    }
}


/* ---------- GStreamer pipeline ---------- */

static GstSample *pop_sample(SpiceGstDecoder *decoder)
{
    g_mutex_lock(&decoder->display_mutex);
    GstSample *sample = g_queue_pop_head(decoder->display_queue);
    g_mutex_unlock(&decoder->display_mutex);
    return sample;
}

static void push_sample_head(SpiceGstDecoder *decoder, GstSample *sample)
{
    g_mutex_lock(&decoder->display_mutex);
    g_queue_push_head(decoder->display_queue, sample);
    g_mutex_unlock(&decoder->display_mutex);
}

static void push_sample_tail(SpiceGstDecoder *decoder, GstSample *sample)
{
    g_mutex_lock(&decoder->display_mutex);
    g_queue_push_tail(decoder->display_queue, sample);
    g_mutex_unlock(&decoder->display_mutex);
}

static void schedule_frame(SpiceGstDecoder *decoder);

/* main context */
static gboolean display_frame(gpointer video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    decoder->timer_id = 0;
    if (decoder->appsink) {
        GstSample *sample = pop_sample(decoder);
        if (!sample) {
            spice_warning("GStreamer error: display_frame() got no frame to display");
            return G_SOURCE_REMOVE;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        SpiceFrameMeta *frame_meta = buffer ? gst_buffer_get_spice_frame_meta(buffer) : NULL;
        GstMapInfo mapinfo;
        if (!frame_meta) {
            spice_warning("GStreamer error: got an unusable sample!");

        } else if (gst_buffer_map(buffer, &mapinfo, GST_MAP_READ)) {
            stream_display_frame(decoder->base.stream, frame_meta->msg, mapinfo.data);
            gst_buffer_unmap(buffer, &mapinfo);

        } else {
            spice_warning("GStreamer error: could not map the buffer");
        }
        gst_sample_unref(sample);
        decoder_unref(decoder);
        schedule_frame(decoder);
    }
    return G_SOURCE_REMOVE;
}

/* main loop or GStreamer streaming thread */
static void schedule_frame(SpiceGstDecoder *decoder)
{
    guint32 time = stream_get_time(decoder->base.stream);
    while (!decoder->timer_id) {
        GstSample *sample = pop_sample(decoder);
        if (!sample) {
            break;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        SpiceFrameMeta *frame_meta = buffer ? gst_buffer_get_spice_frame_meta(buffer) : NULL;
        if (!frame_meta) {
            spice_warning("GStreamer error: got an unusable sample!");
            gst_sample_unref(sample);
            decoder_unref(decoder);
            continue;
        }

        SpiceStreamDataHeader *op = spice_msg_in_parsed(frame_meta->msg);
        if (time < op->multi_media_time) {
            guint32 d = op->multi_media_time - time;
            push_sample_head(decoder, sample);
            decoder->timer_id = g_timeout_add(d, display_frame, decoder);

        } else {
            SPICE_DEBUG("%s: rendering too late by %u ms (ts: %u, mmtime: %u), dropping ",
                        __FUNCTION__, time - op->multi_media_time,
                        op->multi_media_time, time);
            stream_dropped_frame(decoder->base.stream);
            gst_sample_unref(sample);
            decoder_unref(decoder);
        }
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
    if (sample) {
        /* Ensure the video decoder object will still be there when
         * schedule_frame() or display_frame() runs.
         */
        decoder_ref(decoder);
        push_sample_tail(decoder, sample);
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
        SPICE_DEBUG("Unknown codec type %d. Trying decodebin.",
                    decoder->base.codec_type);
        src_caps = "";
        gstdec_name = NULL;
        break;
    }

    /* decodebin will use vaapi if installed, which for a time could
     * intentionally crash the application. So only use decodebin as a
     * fallback or when SPICE_GSTVIDEO_AUTO is set.
     * See: https://bugs.freedesktop.org/show_bug.cgi?id=90884
     */
    if (!gstdec_name || getenv("SPICE_GSTVIDEO_AUTO")) {
        gstdec_name = "decodebin";
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
     * new_sample() call and that any scheduled display_frame() call will
     * not try to use the stream.
     */
    free_pipeline(decoder);

    /* Once we return the stream will be destroyed, preventing any queued
     * frame from being displayed. So clear the frames display queue.
     */
    if (decoder->timer_id) {
        g_source_remove(decoder->timer_id);
        decoder->timer_id = 0;
    }
    GstSample *sample;
    while ((sample = pop_sample(decoder))) {
        gst_sample_unref(sample);
        decoder_unref(decoder);
    }
    g_mutex_clear(&decoder->display_mutex);
    g_queue_free(decoder->display_queue);
    decoder->display_queue = NULL;

    decoder_unref(decoder);

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
    gst_buffer_add_spice_frame_meta(buffer, frame_msg);

    GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_PTS(buffer) = gst_clock_get_time(decoder->clock) - gst_element_get_base_time(decoder->pipeline) + ((uint64_t)latency) * 1000 * 1000;

    if (gst_app_src_push_buffer(decoder->appsrc, buffer) != GST_FLOW_OK) {
        SPICE_DEBUG("GStreamer error: unable to push frame of size %d", size);
        stream_dropped_frame(decoder->base.stream);
    }
}

static gboolean gstvideo_init(void)
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
        decoder->refs = 1;
        g_mutex_init(&decoder->display_mutex);
        decoder->display_queue = g_queue_new();
    }

    return (VideoDecoder*)decoder;
}

G_GNUC_INTERNAL
gboolean gstvideo_has_codec(int codec_type)
{
    gboolean has_codec = FALSE;

    VideoDecoder *decoder = create_gstreamer_decoder(codec_type, NULL);
    if (decoder) {
        has_codec = create_pipeline((SpiceGstDecoder*)decoder);
        decoder->destroy(decoder);
    }

    return has_codec;
}
