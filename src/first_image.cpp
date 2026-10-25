#include "first_image.h"

#include "decoders/decoder.h"
#include "platform.h"

FirstImage::FirstImage(std::string path, std::shared_future<Config> settings)
    : path_(std::move(path))
{
    thread_ = std::thread(
        [this, settings]
        {
            settings.wait();
            platform::trace("first image: decoding starts");
            // A stand-in at most a 4K screen's size: the window, not open
            // yet, is never bigger, and an 8K camera JPEG decoded whole
            // costs hundreds of milliseconds before anything shows.
            set_preview_fit(3840, 2160);
            auto image = std::make_shared<Image>();
            std::string error;
            bool preview = false;
            if (!decode_is_quick(path_) && decode_preview(path_, *image))  // a raw the GPU decodes needs no stand-in
                preview = true;
            else if (!decode_image(path_, *image, error))
                image.reset();
            if (image)
                image->build_mips();
            release_file_bytes();  // this thread's copy of the file
            platform::trace("first image: decoded%s", image ? "" : " (failed)");
            std::function<void()> done;
            {
                std::lock_guard lock(mutex_);
                image_ = std::move(image);
                error_ = std::move(error);
                preview_ = preview;
                done_ = true;
                done = std::move(on_done_);
            }
            cv_.notify_all();
            if (done)
                done();
        });
}

FirstImage::~FirstImage()
{
    if (thread_.joinable())
        thread_.join();
}

bool FirstImage::wait_for(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return done_; });
}

void FirstImage::on_done(std::function<void()> done)
{
    {
        std::lock_guard lock(mutex_);
        if (!done_)
        {
            on_done_ = std::move(done);
            return;
        }
    }
    done();
}
