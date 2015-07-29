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

    GstBuffer *buffer;
    GstMapInfo mapinfo;
} SpiceGstDecoder;


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
        src_caps = "caps=video/x-vp8";
        gstdec_name = "vp8dec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_H264:
        src_caps = "caps=video/x-h264";
        gstdec_name = "h264parse ! avdec_h264";
        break;
    default:
        spice_warning("Unknown codec type %d", decoder->base.codec_type);
        return -1;
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
    decoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "sink"));

    if (gst_element_set_state(decoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        SPICE_DEBUG("GStreamer error: Unable to set the pipeline to the playing state.");
        reset_pipeline(decoder);
        return FALSE;
    }

    return TRUE;
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

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    gst_buffer_fill(buffer, 0, data, size);
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

    if (push_compressed_buffer(decoder, frame_msg)) {
        return pull_raw_frame(decoder);
    }
    return NULL;
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
        decoder->base.destroy = &gst_decoder_destroy;
        decoder->base.decode_frame = &gst_decoder_decode_frame;
        decoder->base.codec_type = codec_type;
        decoder->base.stream = stream;
    }

    return (VideoDecoder*)decoder;
}
