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

    /* ---------- GStreamer pipeline ---------- */

    GstAppSrc *appsrc;
    GstAppSink *appsink;
    GstElement *pipeline;
    GstClock *clock;

    /* ---------- Spice clock data ---------- */

    uint32_t last_mm_time;
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

static void gst_decoder_ref(SpiceGstDecoder *decoder)
{
    g_atomic_int_inc(&decoder->refs);
}

static void gst_decoder_unref(SpiceGstDecoder *decoder)
{
    gint old_refs;
    do {
       old_refs = decoder->refs;
    } while (!g_atomic_int_compare_and_exchange(&decoder->refs, old_refs, old_refs - 1));
    if (old_refs == 1) {
        /* At this point gst_decoder_destroy() has already
         * released all the resources.
         */
        g_free(decoder);
    }
}

/* ---------- GStreamer pipeline ---------- */

/* main context */
static gboolean display_frame(gpointer video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    if (decoder->appsink) {
        GstSample *sample = gst_app_sink_pull_sample(decoder->appsink);
        if (!sample) {
            spice_warning("GStreamer error: could not pull sample");
            gst_decoder_unref(decoder);
            return GST_FLOW_OK;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        SpiceFrameMeta *frame_meta = gst_buffer_get_spice_frame_meta(buffer);
        GstMapInfo mapinfo;
        if (frame_meta && gst_buffer_map(buffer, &mapinfo, GST_MAP_READ)) {
            stream_display_frame(decoder->base.stream, frame_meta->msg, mapinfo.data);
            gst_buffer_unmap(buffer, &mapinfo);
        } else {
            spice_warning("GStreamer error: could not map the buffer");
        }
        gst_sample_unref(sample);
    }
    gst_decoder_unref(decoder);
    return G_SOURCE_REMOVE;
}

/* GStreamer thread */
GstFlowReturn new_sample(GstAppSink *gstappsink, gpointer video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    /* Unfortunately GStreamer's signals are not always run in the main
     * context so they cannot directly display the frame. So use a callback
     * (lower overhead) and bounce the task to the main context through
     * g_timeout_add().
     */

    /* Ensure the video decoder object will still be there when
     * display_frame() runs.
     */
    gst_decoder_ref(decoder);

    g_timeout_add(0, display_frame, decoder);
    return GST_FLOW_OK;
}

static void reset_pipeline(SpiceGstDecoder *decoder)
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

static gboolean construct_pipeline(SpiceGstDecoder *decoder)
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

    /* - Set sync=true so appsink determines when to display the frames.
     * - Set max-buffers=3 to limit the memory hungry buffering of decoded
     *   frames.
     * - Set qos=true on appsink to the elements in the GStreamer pipeline
     *   try to keep up with realtime.
     * - Set drop=false on appsink and block=true on appsrc so that when
     *   the pipeline really cannot keep up delays bubble up the pipeline
     *   all the way to queue_frame() where they may result in potentially
     *   helpful bandwidth adjustments on the Spice server.
     * - Set max-bytes=0 on appsrc so appsrc does not drop frames that may
     *   be needed by those that follow.
     */
    gchar *desc = g_strdup_printf("appsrc name=src is-live=true format=time max-bytes=0 block=true %s ! %s ! videoconvert ! appsink name=sink caps=video/x-raw,format=BGRx sync=true qos=true max-buffers=3 drop=false", src_caps, gstdec_name);
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
        reset_pipeline(decoder);
        return FALSE;
    }

    return TRUE;
}


/* ---------- VideoDecoder's public API ---------- */

static void gst_decoder_reschedule(VideoDecoder *video_decoder)
{
    /* Without knowing how much the clock changed, there is really
     * nothing we can do.
     */
}

/* main context */
static void gst_decoder_destroy(VideoDecoder *video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    /* Reset the pipeline to ensure there will not be any further
     * new_sample() call and to ensure any scheduled display_frame()
     * call will not try to use the stream.
     */
    reset_pipeline(decoder);

    gst_decoder_unref(decoder);
    /* Don't call gst_deinit() as other parts of the client
     * may still be using GStreamer.
     */
}

static void release_buffer_data(gpointer data)
{
    SpiceMsgIn* frame_msg = (SpiceMsgIn*)data;
    spice_msg_in_unref(frame_msg);
}

static void gst_decoder_queue_frame(VideoDecoder *video_decoder,
                                    SpiceMsgIn *frame_msg, int32_t latency)
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
        reset_pipeline(decoder);
    }
    if (!decoder->pipeline && !construct_pipeline(decoder)) {
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
        decoder->base.destroy = &gst_decoder_destroy;
        decoder->base.reschedule = &gst_decoder_reschedule;
        decoder->base.queue_frame = &gst_decoder_queue_frame;
        decoder->base.codec_type = codec_type;
        decoder->base.stream = stream;
        decoder->refs = 1;
    }

    return (VideoDecoder*)decoder;
}

G_GNUC_INTERNAL
gboolean gstvideo_has_codec(int codec_type)
{
    gboolean has_codec = FALSE;

    VideoDecoder *decoder = create_gstreamer_decoder(codec_type, NULL);
    if (decoder) {
        has_codec = construct_pipeline((SpiceGstDecoder*)decoder);
        decoder->destroy(decoder);
    }

    return has_codec;
}
