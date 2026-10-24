#pragma once

#include "encoders/encoder.h"
#include "metadata/file_metadata.h"

#include <rapport/rapport.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Save As, first step: the format, below it that format's own settings, and
// what of the image's metadata to keep. Modeless, so the main view (zoom,
// pan) stays usable for judging a preview. Save… then hands over to the file
// dialog for that format's files.
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
    // `metadata` is the choice to start from; `present` which groups the
    // image has anything in (metadata_present: the rest are greyed).
    static std::shared_ptr<SaveDialog> open(rap::ui& ui, rap::window* parent, const std::string& own_extension,
                                            std::map<std::string, EncodeOptions>& remembered, const MetadataChoice& metadata,
                                            const MetadataChoice& present, Callbacks callbacks);
    MetadataChoice metadata() const;  // as chosen

    void set_info(const std::string& text);  // beside the format: the preview's file size
    // JPEG's Lossless choice: available or not, and a few words (how the
    // result differs from the image, or why it can't be done). Checked to
    // start with when `exact`. The choice reaches `chosen` as "lossless"=1.
    void set_lossless(bool available, bool exact, const std::string& note);
    bool preview_shown() const { return !preview_ || preview_->checked(); }
    void close();                            // as Cancel

private:
    // The control for one setting; one of these is set.
    struct Control
    {
        rap::slider* range = nullptr;
        rap::check* toggle = nullptr;
        rap::drop_down* choice = nullptr;
    };
    struct Page
    {
        const Encoder* encoder;
        rap::widget* box = nullptr;
        std::vector<Control> controls;  // per setting, sized before building
        rap::check* lossless = nullptr;  // JPEG only
    };

    rap::node page_node(Page& page);
    EncodeOptions options(const Page& page) const;
    void update_enabled(Page& page);
    void show_format(int index);
    void report();
    void finish(bool save);
    rap::node metadata_node(const MetadataChoice& initial);
    void preset_chosen(int index);
    void group_toggled();
    void show_choice();
    void update_metadata();  // greyed where the format or the image has none

    rap::ui* ui_ = nullptr;
    std::unique_ptr<rap::window> window_;
    std::vector<Page> pages_;
    rap::drop_down* format_ = nullptr;
    std::map<std::string, EncodeOptions>* remembered_ = nullptr;
    Callbacks callbacks_;
    std::weak_ptr<SaveDialog> self_;
    bool finished_ = false;
    rap::label* info_ = nullptr;
    rap::check* preview_ = nullptr;
    rap::label* lossless_note_ = nullptr;
    // Metadata: a preset, and the groups it stands for (which can be set one by one).
    rap::drop_down* preset_ = nullptr;
    std::vector<rap::check*> groups_;
    rap::label* metadata_note_ = nullptr;
    MetadataChoice present_;
    MetadataChoice choice_;  // as chosen; groups the image lacks keep their setting, shown unchecked
    bool syncing_ = false;
};
