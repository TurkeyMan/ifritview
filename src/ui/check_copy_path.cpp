#include "app.h"
#include "i18n.h"
#include "plugins.h"

#include <cstdio>

// The null toolkit supplies the window and timers. Record clipboard writes
// instead of touching the user's clipboard; exercise the same action body.
int App::check_copy_path(rap::ui& ui)
{
    Config cfg;
    cfg.lens_correction = cfg.lens_downloads = cfg.animate = false;
    cfg.map_tile_url.clear();
    cfg.map_satellite_url.clear();
    cfg.geocoder_url.clear();
    std::promise<Config> settings;
    settings.set_value(cfg);
    // Drain plugin arrival while there is no synthetic network folder to scan.
    plugins::wait();
    App app(ui, settings.get_future().share());
    ui.run();
    struct Label : rap::label
    {
        std::string text, tooltip;
        bool enabled = true;
        void set_enabled(bool v) override { enabled = v; }
        void set_visible(bool) override {}
        void set_tooltip(const std::string& v) override { tooltip = v; }
        void focus() override {}
        bool has_focus() const override { return false; }
        void set_text(const std::string& v) override { text = v; }
    } filename, notice, message;
    auto* original_filename = app.status_left_;
    auto* original_notice = app.status_notice_;
    auto* original_message = app.status_message_label_;
    app.status_left_ = &filename;
    app.status_notice_ = &notice;
    app.status_message_label_ = &message;
    int failed = 0, writes = 0;
    std::string clipboard;
    const auto expect = [&](bool ok, const char* what)
    {
        std::printf("copy-path: %s: %s\n", what, ok ? "ok" : "FAILED");
        failed += !ok;
    };
    const auto write = [&](const std::string& path) { ++writes; clipboard = path; };
    // Invoke the exact callback installed in the production icon. The plain
    // filename deliberately retains its shrink/ellipsis behavior on Win32.
    auto name_node = app.status_filename();
    const auto& label = std::get<rap::spec::label>(name_node.spec);
    expect(label.ellipsize && !label.link && !label.on_click, "filename retains plain ellipsis behavior");
    auto button = app.status_copy_button(write);
    const auto click_icon = std::get<rap::spec::tool_button>(button.spec).on_click;
    click_icon();
    expect(writes == 0, "no selected file leaves clipboard untouched");

    // Windows-style UNC text is meaningful on Windows; on Unix exercise its
    // native network-path spelling. Neither goes through canonicalization.
#ifdef _WIN32
    const std::string dir = "\\\\server\\share\\Photos";
    const std::string separator = "\\";
#else
    const std::string dir = "//server/share/Photos";
    const std::string separator = "/";
#endif
    const std::string name = "Hanna's graduation + Noosa \xc3\xa9.CR2";
    const std::string path = dir + separator + name;
    app.folder_.adopt({dir, name, true, {name, "next.CR3"}});
    app.image_path_ = "wrong-preview-cache-file.jpg";
    app.status_message_ = "busy fixture";
    app.busy_message_ = app.status_message_;
    click_icon();
    expect(writes == 1 && clipboard == path, "full UTF-8 network path copied verbatim, not preview/cache path");
    expect(filename.tooltip == path && filename.enabled, "filename tooltip contains the full selected path");
    expect(notice.text == _("Copied") && message.text == app.busy_message_ && app.status_message_ == app.busy_message_,
           "confirmation preserves develop message and busy state");
    ui.run(); // The null toolkit fires each timer once, without waiting.
    expect(app.copied_path_.empty() && notice.text.empty(), "confirmation expires");

    click_icon();
    click_icon(); // replace the first timer
    ui.run();
    expect(writes == 3 && app.copied_path_.empty(), "repeated icon clicks replace the confirmation timer");
    app.copy_file_path_to(write);
    app.folder_.step(1, false);
    app.update_status();
    expect(app.copied_path_.empty(), "navigation clears the old file's confirmation");
    app.copy_file_path_to(write);
    expect(clipboard == dir + separator + "next.CR3", "copy follows the selected file during navigation");
    auto pasted = std::make_shared<Image>();
    pasted->allocate(2, 2);
    app.show_pasted(pasted); // Retains the folder cursor for later navigation.
    expect(!app.folder_.empty() && app.pasted_, "paste keeps the old folder selection for navigation");
    const int before = writes;
    app.copy_file_path_to(write);
    expect(writes == before && app.status_file_path().empty() && app.copied_path_.empty(), "clipboard image has no file path or confirmation");
    expect(!filename.enabled && filename.tooltip.empty(), "clipboard filename is disabled and has no path tooltip");
    app.pasted_ = false;
    app.copy_file_path_to(write);
    app.quit();
    expect(!app.copied_timer_, "quit cancels the pending confirmation");
    app.status_left_ = original_filename;
    app.status_notice_ = original_notice;
    app.status_message_label_ = original_message;
    return failed ? 1 : 0;
}
