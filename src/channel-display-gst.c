/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 CodeWeavers, Inc

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

    /* ---------- GStreamer pipeline ---------- */

    GstElement *pipeline;
    GstAppSrc *appsrc;
    GstAppSink *appsink;

    /* ---------- Output frame data ---------- */

    GMutex pipeline_mutex;
    GCond pipeline_cond;
    gboolean pipeline_wait;
    uint32_t samples_count;

    GstBuffer *buffer;
    GstMapInfo mapinfo;
} SpiceGstDecoder;


/* Signals that the pipeline is done processing the last buffer we gave it.
 *
 * @decoder:   The video decoder object.
 * @samples:   How many samples to add to the available samples count.
 */
static void signal_pipeline(SpiceGstDecoder *decoder, uint32_t samples)
{
    g_mutex_lock(&decoder->pipeline_mutex);
    decoder->pipeline_wait = FALSE;
    decoder->samples_count += samples;
    g_cond_signal(&decoder->pipeline_cond);
    g_mutex_unlock(&decoder->pipeline_mutex);
}

static void appsrc_need_data_cb(GstAppSrc *src, guint length, gpointer user_data)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)user_data;
    signal_pipeline(decoder, 0);
}

static GstFlowReturn appsink_new_sample_cb(GstAppSink *appsrc, gpointer user_data)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)user_data;
    signal_pipeline(decoder, 1);
    return GST_FLOW_OK;
}

/* ---------- GStreamer pipeline ---------- */

static void reset_pipeline(SpiceGstDecoder *decoder)
{
    if (!decoder->pipeline) {
        return;
    }

    gst_element_set_state(decoder->pipeline, GST_STATE_NULL);
    gst_object_unref(decoder->appsrc);
    gst_object_unref(decoder->appsink);
    gst_object_unref(decoder->pipeline);
    decoder->pipeline = NULL;

    g_mutex_clear(&decoder->pipeline_mutex);
    g_cond_clear(&decoder->pipeline_cond);
}

static gboolean construct_pipeline(SpiceGstDecoder *decoder)
{
    g_mutex_init(&decoder->pipeline_mutex);
    g_cond_init(&decoder->pipeline_cond);
    decoder->pipeline_wait = TRUE;
    decoder->samples_count = 0;

    /* Note that we cannot rely on typefind to always identify the stream type
     * (e.g. VP8). So always set the source caps for streams we know about.
     * See: https://bugzilla.gnome.org/show_bug.cgi?id=756457
     *
     * Furthermore decodebin will use vaapi if installed, which for a time
     * could intentionally crash the application. So only use decodebin as a
     * fallback for now. See: https://bugs.freedesktop.org/show_bug.cgi?id=90884
     */
    const gchar *src_caps;
    const gchar *gstdec_name = NULL;
    switch (decoder->base.codec_type) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        src_caps = "caps=image/jpeg";
        gstdec_name = "jpegdec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        src_caps = "caps=video/x-vp8";
        gstdec_name = "vp8dec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_H264:
        src_caps = "caps=video/x-h264";
        gstdec_name = "h264parse ! avdec_h264";
        break;
    default:
        SPICE_DEBUG("Unknown codec type %d. Trying decodebin.",
                    decoder->base.codec_type);
        src_caps = "";
        break;
    }
    /* Set SPICE_GSTVIDEO_AUTO to test decodebin and maybe get hardware acceleration */
    if (!gstdec_name || getenv("SPICE_GSTVIDEO_AUTO")) {
        gstdec_name = "decodebin";
    }

    GError *err = NULL;
    gchar *desc = g_strdup_printf("appsrc name=src format=2 do-timestamp=1 %s ! %s ! videoconvert ! appsink name=sink caps=video/x-raw,format=BGRx", src_caps, gstdec_name);
    SPICE_DEBUG("GStreamer pipeline: %s", desc);
    decoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    g_free(desc);
    if (!decoder->pipeline) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    decoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "src"));
    GstAppSrcCallbacks appsrc_cbs = {&appsrc_need_data_cb, NULL, NULL, {NULL}};
    gst_app_src_set_callbacks(decoder->appsrc, &appsrc_cbs, decoder, NULL);

    decoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "sink"));
    GstAppSinkCallbacks appsink_cbs = {NULL, NULL, &appsink_new_sample_cb, {NULL}};
    gst_app_sink_set_callbacks(decoder->appsink, &appsink_cbs, decoder, NULL);

    if (gst_element_set_state(decoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        SPICE_DEBUG("GStreamer error: Unable to set the pipeline to the playing state.");
        reset_pipeline(decoder);
        return FALSE;
    }

    return TRUE;
}

static void release_msg_in(gpointer data)
{
    spice_msg_in_unref((SpiceMsgIn*)data);
}

static gboolean push_compressed_buffer(SpiceGstDecoder *decoder,
                                       SpiceMsgIn *frame_msg)
{
    uint8_t *data;
    uint32_t size = spice_msg_in_frame_data(frame_msg, &data);
    if (size == 0) {
        SPICE_DEBUG("got an empty frame buffer!");
        return FALSE;
    }

    /* Reference frame_msg so it stays around until our 'deallocator' releases it */
    spice_msg_in_ref(frame_msg);
    GstBuffer *buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
                                                    data, size, 0, size,
                                                    frame_msg, &release_msg_in);

    if (gst_app_src_push_buffer(decoder->appsrc, buffer) != GST_FLOW_OK) {
        SPICE_DEBUG("GStreamer error: unable to push frame of size %d", size);
        return FALSE;
    }

    return TRUE;
}

static void release_last_frame(SpiceGstDecoder *decoder)
{
    if (decoder->buffer) {
        if (decoder->mapinfo.memory) {
            gst_buffer_unmap(decoder->buffer, &decoder->mapinfo);
            decoder->mapinfo.memory = NULL;
        }
        gst_buffer_unref(decoder->buffer);
        decoder->buffer = NULL;
    }
}

static uint8_t* pull_raw_frame(SpiceGstDecoder *decoder)
{
    GstSample *sample = gst_app_sink_pull_sample(decoder->appsink);
    if (!sample) {
        SPICE_DEBUG("GStreamer error: could not pull sample");
        return NULL;
    }
    decoder->buffer = gst_sample_get_buffer(sample);
    gst_buffer_ref(decoder->buffer);
    gst_sample_unref(sample);

    if (gst_buffer_map(decoder->buffer, &decoder->mapinfo, GST_MAP_READ)) {
        return decoder->mapinfo.data;
    }

    SPICE_DEBUG("GStreamer error: could not map the buffer");
    gst_buffer_unref(decoder->buffer);
    decoder->buffer = NULL;
    decoder->mapinfo.memory = NULL;
    return NULL;
}


/* ---------- VideoDecoder's public API ---------- */

static void gst_decoder_destroy(VideoDecoder *video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;
    release_last_frame(decoder);
    reset_pipeline(decoder);
    g_free(decoder);
    /* Don't call gst_deinit() as other parts may still be using GStreamer */
}

static uint8_t* gst_decoder_decode_frame(VideoDecoder *video_decoder,
                                         SpiceMsgIn *frame_msg)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;
    int width, height;

    stream_get_dimensions(decoder->base.stream, frame_msg, &width, &height);
    if (width != decoder->width || height != decoder->height) {
        SPICE_DEBUG("video format change: width %d -> %d, height %d -> %d", decoder->width, width, decoder->height, height);
        decoder->width = width;
        decoder->height = height;
        reset_pipeline(decoder);
    }
    if (!decoder->pipeline && !construct_pipeline(decoder)) {
        return NULL;
    }

    /* Release the output frame buffer early so the pipeline can reuse it.
     * This also simplifies error handling.
     */
    release_last_frame(decoder);

    /* The pipeline may have called appsrc_need_data_cb() after we got the last
     * output frame. This would cause us to return prematurely so reset
     * pipeline_wait so we do wait for it to process this buffer.
     */
    g_mutex_lock(&decoder->pipeline_mutex);
    decoder->pipeline_wait = TRUE;
    g_mutex_unlock(&decoder->pipeline_mutex);
    /* Note that it's possible for appsrc_need_data_cb() to get called between
     * now and the pipeline wait. But this will at most cause a one frame delay.
     */

    if (push_compressed_buffer(decoder, frame_msg)) {
        /* Wait for the pipeline to either produce a decoded frame, or ask
         * for more data which means an error happened.
         */
        g_mutex_lock(&decoder->pipeline_mutex);
        while (decoder->pipeline_wait) {
            g_cond_wait(&decoder->pipeline_cond, &decoder->pipeline_mutex);
        }
        decoder->pipeline_wait = TRUE;
        uint32_t samples = decoder->samples_count;
        if (samples) {
            decoder->samples_count--;
        }
        g_mutex_unlock(&decoder->pipeline_mutex);

        /* If a decoded frame waits for us, return it */
        if (samples) {
            return pull_raw_frame(decoder);
        }
    }
    return NULL;
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
        decoder->base.decode_frame = &gst_decoder_decode_frame;
        decoder->base.codec_type = codec_type;
        decoder->base.stream = stream;
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
