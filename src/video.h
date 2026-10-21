#pragma once

#include "image.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Plays a video into an Image: GStreamer decodes (and rotates, per the file's
// orientation tag) frames as BGRA, which replace the image's pixels on the
// main thread. Everything that shows an image (zoom, fullscreen, overlay)
// therefore shows the video. Stops on the last frame, or loops if asked.
class VideoPlayer
{
public:
    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    void start(const std::string& path, std::shared_ptr<Image> target, bool muted, bool loop);
    void stop();
    bool active() const;
    bool playing() const { return playing_; }
    void toggle();
    void set_playing(bool playing);
    // Exchange playbacks with `other` (frames keep flowing to the same image).
    void swap(VideoPlayer& other);
    void set_muted(bool muted);
    void set_volume(double volume);  // 0..1, perceptual
    void set_loop(bool loop);
    // `final`: the end of a drag; seek exactly rather than to the nearest key frame.
    void seek(double fraction, bool final);
    double position() const;  // seconds
    double duration() const;  // seconds; 0 if unknown

    std::function<void()> on_frame;  // main thread, after the image's pixels changed

    struct Impl;  // GStreamer state; public so its C callbacks can reach it

private:
    Impl* impl_ = nullptr;
    bool playing_ = false;
};

// Whether videos can play: built with video support and GStreamer present
// at runtime (it's loaded on first use, never linked).
bool video_supported();
// The extensions handled as video; without GStreamer the series skips them.
const std::vector<std::string>& video_extensions();
bool is_video_extension(const std::string& ext);
