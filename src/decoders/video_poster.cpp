// Videos in the series: the first frame (upright, per the rotation tag) is
// the image; the player takes over once it's on screen. GStreamer reads the
// file itself, so this decoder is marked streaming and gets no preload.
#include "decoder.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <cstdio>
#include <cstring>

namespace
{

std::string clock_text(double seconds)
{
    const int s = int(seconds + 0.5);
    char buf[32];
    if (s >= 3600)
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

std::string tag_string(GstTagList* tags, const char* tag)
{
    gchar* v = nullptr;
    if (!tags || !gst_tag_list_get_string(tags, tag, &v))
        return "";
    std::string s = v;
    g_free(v);
    return s;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    static const bool initialised = gst_init_check(nullptr, nullptr, nullptr);
    if (!initialised)
    {
        error = "GStreamer unavailable";
        return false;
    }
    GstElement* playbin = gst_element_factory_make("playbin", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    GstElement* audio = gst_element_factory_make("fakesink", nullptr);
    if (!playbin || !sink || !audio)
    {
        error = "GStreamer playbin missing";
        return false;
    }
    GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA");
    g_object_set(sink, "caps", caps, "sync", FALSE, nullptr);
    gst_caps_unref(caps);
    if (GstElement* flip = gst_element_factory_make("videoflip", nullptr))
    {
        gst_util_set_object_arg(G_OBJECT(flip), "method", "automatic");
        g_object_set(playbin, "video-filter", flip, nullptr);
    }
    gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
    g_object_set(playbin, "uri", uri, "video-sink", sink, "audio-sink", audio, nullptr);
    g_free(uri);

    bool ok = false;
    gst_element_set_state(playbin, GST_STATE_PAUSED);
    if (GstSample* sample = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 10 * GST_SECOND))
    {
        GstVideoInfo info;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) && gst_buffer_map(buffer, &map, GST_MAP_READ))
        {
            const int w = GST_VIDEO_INFO_WIDTH(&info), h = GST_VIDEO_INFO_HEIGHT(&info);
            const int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
            out.allocate(w, h);
            for (int y = 0; y < h; ++y)
                std::memcpy(out.row(y), map.data + size_t(y) * stride, size_t(w) * 4);
            gst_buffer_unmap(buffer, &map);
            if (GST_VIDEO_INFO_FPS_N(&info) > 0 && GST_VIDEO_INFO_FPS_D(&info) > 0)
            {
                char fps[32];
                std::snprintf(fps, sizeof fps, "%.3g fps", double(GST_VIDEO_INFO_FPS_N(&info)) / GST_VIDEO_INFO_FPS_D(&info));
                out.add("Video", "Frame rate", fps);
            }
            ok = true;
        }
        gst_sample_unref(sample);
    }
    if (ok)
    {
        out.is_video = true;
        gint64 t = 0;
        if (gst_element_query_duration(playbin, GST_FORMAT_TIME, &t) && t > 0)
            out.duration = double(t) / GST_SECOND;
        GstTagList* vtags = nullptr;
        GstTagList* atags = nullptr;
        g_signal_emit_by_name(playbin, "get-video-tags", 0, &vtags);
        g_signal_emit_by_name(playbin, "get-audio-tags", 0, &atags);
        const std::string vcodec = tag_string(vtags, GST_TAG_VIDEO_CODEC);
        const std::string acodec = tag_string(atags, GST_TAG_AUDIO_CODEC);
        out.format = vcodec.empty() ? "Video" : "Video, " + vcodec;
        out.add("Video", "Duration", out.duration > 0 ? clock_text(out.duration) : "unknown");
        out.add("Video", "Size", std::to_string(out.width) + " × " + std::to_string(out.height));
        out.add("Video", "Codec", vcodec);
        out.add("Video", "Audio", acodec.empty() ? (atags ? "yes" : "none") : acodec);
        const std::string container = tag_string(vtags, GST_TAG_CONTAINER_FORMAT);
        out.add("Video", "Container", container);
        out.info = out.duration > 0 ? clock_text(out.duration) : "";
        out.pixel_format.notes.push_back(vcodec.empty() ? "video" : vcodec);
        if (vtags)
            gst_tag_list_unref(vtags);
        if (atags)
            gst_tag_list_unref(atags);
    }
    else
        error = "no video frame";
    gst_element_set_state(playbin, GST_STATE_NULL);
    gst_object_unref(playbin);
    return ok;
}

}  // namespace

const Decoder& video_decoder()
{
    static const Decoder d{"Video", {"mp4", "m4v", "mov", "mts", "m2ts", "mkv", "webm", "avi", "3gp"}, nullptr, decode, nullptr, true};
    return d;
}
