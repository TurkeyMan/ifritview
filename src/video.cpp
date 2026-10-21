#include "video.h"

#if defined(HAVE_VIDEO) && !defined(_WIN32)  // Windows: video_mf.cpp

#include "gst_api.h"

#include <algorithm>
#include <cstring>
#include <mutex>

struct VideoPlayer::Impl
{
    VideoPlayer* owner = nullptr;
    GstElement* playbin = nullptr;
    GstElement* sink = nullptr;
    guint bus_watch = 0;
    std::shared_ptr<Image> target;
    bool loop = false;

    // Hand-off from the streaming thread to the main thread.
    std::mutex mutex;
    PixelBuffer frame;
    int frame_w = 0, frame_h = 0;
    guint idle = 0;

    static GstFlowReturn on_sample(GstAppSink* sink, gpointer self, bool preroll)
    {
        auto* impl = static_cast<Impl*>(self);
        GstSample* sample = preroll ? gst_app_sink_pull_preroll(sink) : gst_app_sink_pull_sample(sink);
        if (!sample)
            return GST_FLOW_OK;
        GstVideoInfo info;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) && gst_buffer_map(buffer, &map, GST_MAP_READ))
        {
            const int w = GST_VIDEO_INFO_WIDTH(&info), h = GST_VIDEO_INFO_HEIGHT(&info);
            const int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
            std::lock_guard lock(impl->mutex);
            impl->frame.resize(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y)
                std::memcpy(impl->frame.data() + size_t(y) * w * 4, map.data + size_t(y) * stride, size_t(w) * 4);
            impl->frame_w = w;
            impl->frame_h = h;
            if (!impl->idle)
                impl->idle = g_idle_add_full(G_PRIORITY_HIGH, &Impl::apply, impl, nullptr);
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Main thread: the newest frame becomes the image's pixels.
    static gboolean apply(gpointer self)
    {
        auto* impl = static_cast<Impl*>(self);
        {
            std::lock_guard lock(impl->mutex);
            impl->idle = 0;
            if (!impl->target || impl->frame.empty())
                return G_SOURCE_REMOVE;
            Image& img = *impl->target;
            if (img.width != impl->frame_w || img.height != impl->frame_h)
                img.allocate(impl->frame_w, impl->frame_h);
            std::memcpy(img.pixels.data(), impl->frame.data(), img.pixels.size());
            img.mips.clear();
        }
        if (impl->owner->on_frame)
            impl->owner->on_frame();
        return G_SOURCE_REMOVE;
    }

    static gboolean on_bus(GstBus*, GstMessage* msg, gpointer self)
    {
        auto* impl = static_cast<Impl*>(self);
        switch (GST_MESSAGE_TYPE(msg))
        {
            case GST_MESSAGE_EOS:
                if (impl->loop)
                    gst_element_seek_simple(impl->playbin, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
                else
                {
                    gst_element_set_state(impl->playbin, GST_STATE_PAUSED);  // stays on the last frame
                    impl->owner->playing_ = false;
                    if (impl->owner->on_frame)
                        impl->owner->on_frame();
                }
                break;
            case GST_MESSAGE_ERROR:
            {
                GError* err = nullptr;
                gst_message_parse_error(msg, &err, nullptr);
                g_warning("video: %s", err ? err->message : "error");
                if (err)
                    g_error_free(err);
                gst_element_set_state(impl->playbin, GST_STATE_NULL);
                impl->owner->playing_ = false;
                break;
            }
            default:
                break;
        }
        return G_SOURCE_CONTINUE;
    }
};

bool video_available()
{
    return gst_api_available();
}

bool video_supported()
{
    return gst_api_load();
}

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer()
{
    stop();
}

void VideoPlayer::start(const std::string& path, std::shared_ptr<Image> target, bool muted, bool loop)
{
    stop();
    if (!gst_api_load())
        return;
    impl_ = new Impl;
    impl_->owner = this;
    impl_->target = std::move(target);
    impl_->loop = loop;
    impl_->playbin = gst_element_factory_make("playbin", nullptr);
    impl_->sink = gst_element_factory_make("appsink", nullptr);
    if (!impl_->playbin || !impl_->sink)
    {
        stop();
        return;
    }
    GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA");  // cairo's ARGB32 byte order
    g_object_set(impl_->sink, "caps", caps, "sync", TRUE, "max-buffers", 2u, "drop", TRUE, nullptr);
    gst_caps_unref(caps);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = [](GstAppSink* s, gpointer self) { return Impl::on_sample(s, self, false); };
    callbacks.new_preroll = [](GstAppSink* s, gpointer self) { return Impl::on_sample(s, self, true); };
    gst_app_sink_set_callbacks(GST_APP_SINK(impl_->sink), &callbacks, impl_, nullptr);
    // Phones record sideways with a rotation tag; turn the frames upright.
    if (GstElement* flip = gst_element_factory_make("videoflip", nullptr))
    {
        gst_util_set_object_arg(G_OBJECT(flip), "method", "automatic");
        g_object_set(impl_->playbin, "video-filter", flip, nullptr);
    }
    gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
    g_object_set(impl_->playbin, "uri", uri, "video-sink", impl_->sink, "mute", muted, nullptr);
    g_free(uri);
    GstBus* bus = gst_element_get_bus(impl_->playbin);
    impl_->bus_watch = gst_bus_add_watch(bus, &Impl::on_bus, impl_);
    gst_object_unref(bus);
    gst_element_set_state(impl_->playbin, GST_STATE_PLAYING);
    playing_ = true;
}

void VideoPlayer::stop()
{
    if (!impl_)
        return;
    if (impl_->playbin)
        gst_element_set_state(impl_->playbin, GST_STATE_NULL);  // joins the streaming threads
    if (impl_->bus_watch)
        g_source_remove(impl_->bus_watch);
    if (impl_->idle)
        g_source_remove(impl_->idle);
    if (impl_->playbin)
        gst_object_unref(impl_->playbin);  // owns the sink
    else if (impl_->sink)
        gst_object_unref(impl_->sink);
    delete impl_;
    impl_ = nullptr;
    playing_ = false;
}

bool VideoPlayer::active() const
{
    return impl_ != nullptr;
}

void VideoPlayer::toggle()
{
    if (!impl_)
        return;
    // At the end, play starts again from the beginning.
    if (!playing_ && duration() > 0 && position() >= duration() - 0.05)
        gst_element_seek_simple(impl_->playbin, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
    playing_ = !playing_;
    gst_element_set_state(impl_->playbin, playing_ ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

void VideoPlayer::swap(VideoPlayer& other)
{
    std::swap(impl_, other.impl_);
    std::swap(playing_, other.playing_);
    if (impl_)
        impl_->owner = this;
    if (other.impl_)
        other.impl_->owner = &other;
}

void VideoPlayer::set_playing(bool playing)
{
    if (!impl_ || playing == playing_)
        return;
    playing_ = playing;
    gst_element_set_state(impl_->playbin, playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

void VideoPlayer::set_muted(bool muted)
{
    if (impl_)
        g_object_set(impl_->playbin, "mute", muted, nullptr);
}

void VideoPlayer::set_loop(bool loop)
{
    if (impl_)
        impl_->loop = loop;
}

void VideoPlayer::set_volume(double volume)
{
    if (impl_)
        gst_stream_volume_set_volume(GST_STREAM_VOLUME(impl_->playbin), GST_STREAM_VOLUME_FORMAT_CUBIC, std::clamp(volume, 0.0, 1.0));
}

void VideoPlayer::seek(double fraction, bool final)
{
    const double d = duration();
    if (!impl_ || d <= 0)
        return;
    const gint64 t = gint64(std::clamp(fraction, 0.0, 1.0) * d * GST_SECOND);
    const auto flags = final ? GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE)
                             : GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST);
    gst_element_seek_simple(impl_->playbin, GST_FORMAT_TIME, flags, t);
}

double VideoPlayer::position() const
{
    gint64 t = 0;
    if (impl_ && gst_element_query_position(impl_->playbin, GST_FORMAT_TIME, &t))
        return double(t) / GST_SECOND;
    return 0;
}

double VideoPlayer::duration() const
{
    gint64 t = 0;
    if (impl_ && gst_element_query_duration(impl_->playbin, GST_FORMAT_TIME, &t))
        return double(t) / GST_SECOND;
    return 0;
}

#elif !defined(HAVE_VIDEO)

struct VideoPlayer::Impl
{
};

bool video_available()
{
    return false;
}
bool video_supported()
{
    return false;
}
VideoPlayer::VideoPlayer() = default;
VideoPlayer::~VideoPlayer() = default;
void VideoPlayer::start(const std::string&, std::shared_ptr<Image>, bool, bool) {}
void VideoPlayer::stop() {}
bool VideoPlayer::active() const
{
    return false;
}
void VideoPlayer::toggle() {}
void VideoPlayer::set_playing(bool) {}
void VideoPlayer::swap(VideoPlayer&) {}
void VideoPlayer::set_muted(bool) {}
void VideoPlayer::set_loop(bool) {}
void VideoPlayer::set_volume(double) {}
void VideoPlayer::seek(double, bool) {}
double VideoPlayer::position() const
{
    return 0;
}
double VideoPlayer::duration() const
{
    return 0;
}

#endif

const std::vector<std::string>& video_extensions()
{
    static const std::vector<std::string> list{"mp4", "m4v", "mov", "mts", "m2ts", "mkv", "webm", "avi", "3gp"};
    return list;
}
