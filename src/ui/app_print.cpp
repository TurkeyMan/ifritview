// Print: the image as shown (or the selection), fitted to the paper and
// centred, through the platform's print dialog. Part of App (app.h).
#include "i18n.h"
#include "app.h"
#include "scale.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

void App::print_image()
{
    if (!image_ || image_->width <= 0 || image_->height <= 0)
        return;
    const std::shared_ptr<Image> edited = edited_image();
    // A selection prints alone, as Ctrl+C copies it.
    std::shared_ptr<const Image> image = edited;
    if (int x, y, w, h; view_->selection(x, y, w, h))
        image = std::make_shared<const Image>(edited->crop(x, y, w, h));

    rap::print_job job;
    job.title = std::filesystem::path(image_path_).filename().string();
    // Turned to the image's shape; the dialog can still change it.
    job.orientation = image->width > image->height ? rap::page_orientation::landscape : rap::page_orientation::portrait;
    job.draw_page = [this, image](rap::painter& p, int, rap::size_f area)
    {
        const double s = std::min(area.w / image->width, area.h / image->height);
        const float w = float(image->width * s), h = float(image->height * s);
        // The pixels the printer's resolution takes, reduced with the
        // viewer's own filter: the printing path's scaling only averages two.
        const int pw = std::max(1, int(std::lround(w * p.device_scale()))), ph = std::max(1, int(std::lround(h * p.device_scale())));
        std::unique_ptr<rap::image> pixels;
        if (pw < image->width && ph < image->height)
        {
            const PixelBuffer small = downscale(*image, pw, ph);
            pixels = ui_.create_image(pw, ph, rap::pixel_format::bgra8_premultiplied, small.data(), size_t(pw) * 4);
        }
        else
        {
            pixels = ui_.create_image(image->width, image->height, rap::pixel_format::bgra8_premultiplied, image->pixels.data(), image->stride);
        }
        p.image(*pixels, {0, 0, float(pixels->width()), float(pixels->height())}, {(area.w - w) / 2, (area.h - h) / 2, w, h}, rap::filter::linear);
    };
    job.done = [this](bool printed, std::string error)
    {
        if (printed)
            status_message_ = _("Printed");
        else if (!error.empty())
        {
            // The reason is the message ("printing isn't available..."), as a sentence.
            status_message_ = error;
            if (status_message_[0] >= 'a' && status_message_[0] <= 'z')
                status_message_[0] = char(status_message_[0] - 'a' + 'A');
        }
        update_status();
    };
    ui_.print(window_.get(), std::move(job));
}
