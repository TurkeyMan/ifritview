#pragma once

#include "encoders/encoder.h"

#include <rapport/rapport.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Save As, first step: the format, and below it that format's own settings.
// Modeless, so the main view (zoom, pan) stays usable for judging a preview.
// Save… then hands over to the file dialog for that format's files.
class SaveDialog
{
public:
    struct Callbacks
    {
        // A format or setting changed (and once at the start): for the preview.
        std::function<void(const Encoder&, const EncodeOptions&)> changed;
        // The Preview toggle: show the preview or the image (no re-encode).
        std::function<void(bool)> preview_shown;
        // Save… was pressed.
        std::function<void(const Encoder&, const EncodeOptions&)> chosen;
        // The dialog has gone, either way.
        std::function<void()> closed;
    };

    // `own_extension` (lower case, "jpg" for JPEG) is preselected when
    // there's an encoder for it. `remembered` keeps each format's settings
    // between saves; it must outlive the dialog.
    static std::shared_ptr<SaveDialog> open(rap::Ui& ui, rap::Window* parent, const std::string& own_extension,
                                            std::map<std::string, EncodeOptions>& remembered, Callbacks callbacks);

    void set_info(const std::string& text);  // beside the format: the preview's file size
    bool preview_shown() const { return !preview_ || preview_->checked(); }
    void close();                            // as Cancel

private:
    // The control for one setting; one of these is set.
    struct Control
    {
        rap::Slider* range = nullptr;
        rap::Check* toggle = nullptr;
        rap::Select* choice = nullptr;
    };
    struct Page
    {
        const Encoder* encoder;
        rap::Widget* box = nullptr;
        std::vector<Control> controls;  // per setting, sized before building
    };

    rap::Node page_node(Page& page);
    EncodeOptions options(const Page& page) const;
    void update_enabled(Page& page);
    void show_format(int index);
    void report();
    void finish(bool save);

    rap::Ui* ui_ = nullptr;
    std::unique_ptr<rap::Window> window_;
    std::vector<Page> pages_;
    rap::Select* format_ = nullptr;
    std::map<std::string, EncodeOptions>* remembered_ = nullptr;
    Callbacks callbacks_;
    std::weak_ptr<SaveDialog> self_;
    bool finished_ = false;
    rap::Label* info_ = nullptr;
    rap::Check* preview_ = nullptr;
};
