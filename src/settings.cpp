#include "settings.h"

#include "config.h"
#include "decoders/decoder.h"
#include "integration.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>

namespace {

const struct { const char* name; const char* url; } kMapProviders[] = {
    {"Google Maps", "https://www.google.com/maps?q={lat},{lon}"},
    {"OpenStreetMap", "https://www.openstreetmap.org/?mlat={lat}&mlon={lon}#map=16/{lat}/{lon}"},
    {"Apple Maps", "https://maps.apple.com/?ll={lat},{lon}&q={lat},{lon}"},
    {"Bing Maps", "https://www.bing.com/maps?cp={lat}~{lon}&lvl=16&sp=point.{lat}_{lon}_Photo"},
};

struct Widgets {
    GtkWidget* fit_window;
    GtkWidget* min_width;
    GtkWidget* min_height;
    GtkWidget* wrap_around;
    GtkWidget* show_hidden;
    GtkWidget* raw_mode;
    GtkWidget* apply_xmp;
    GtkWidget* smooth_upscale;
    GtkWidget* smooth_downscale;
    GtkWidget* fullscreen_upscale;
    GtkWidget* checkerboard;
    GtkWidget* background;
    GtkWidget* fullscreen_background;
    GtkWidget* extensions;   // GtkTextView
    GtkWidget* map_provider;
    GtkWidget* map_url;
    std::array<GtkWidget*, size_t(Control::Count)> controls;
};

GtkWidget* page_grid() {
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 18);
    gtk_widget_set_margin_end(grid, 18);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);
    return grid;
}

GtkWidget* add_heading(GtkWidget* grid, int& row, const char* text) {
    GtkWidget* label = gtk_label_new(nullptr);
    gchar* markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    if (row > 0) gtk_widget_set_margin_top(label, 10);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row++, 2, 1);
    return label;
}

GtkWidget* add_check(GtkWidget* grid, int& row, const char* label, bool value) {
    GtkWidget* check = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), value);
    gtk_grid_attach(GTK_GRID(grid), check, 0, row++, 2, 1);
    return check;
}

void add_row(GtkWidget* grid, int& row, const char* label, GtkWidget* widget) {
    GtkWidget* l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);
}

GtkWidget* spin(int value, int min, int max, int step) {
    GtkWidget* s = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    return s;
}

GtkWidget* color_button(const std::string& hex) {
    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, hex.c_str())) gdk_rgba_parse(&rgba, "#000000");
    return gtk_color_button_new_with_rgba(&rgba);
}

std::string color_hex(GtkWidget* button) {
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &c);
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", int(c.red * 255 + 0.5), int(c.green * 255 + 0.5), int(c.blue * 255 + 0.5));
    return buf;
}

bool checked(GtkWidget* w) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)); }

std::string join_extensions(const std::vector<std::string>& exts) {
    std::string s;
    for (const std::string& e : exts) {
        if (!s.empty()) s += ' ';
        s += e;
    }
    return s;
}

std::vector<std::string> parse_extensions(const std::string& text) {
    std::vector<std::string> out;
    std::string token;
    auto flush = [&] {
        if (!token.empty() && std::find(out.begin(), out.end(), token) == out.end()) out.push_back(token);
        token.clear();
    };
    for (unsigned char c : text) {
        if (std::isspace(c) || c == ',' || c == ';' || c == '.' || c == '*') flush();
        else token += char(std::tolower(c));
    }
    flush();
    return out;
}

void set_text_view(GtkWidget* view, const std::string& text) {
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text.c_str(), -1);
}

std::string get_text_view(GtkWidget* view) {
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::string s = text ? text : "";
    g_free(text);
    return s;
}

GtkWidget* build_general(const Config& cfg, Widgets& w) {
    GtkWidget* grid = page_grid();
    int row = 0;
    add_heading(grid, row, "Window");
    w.fit_window = add_check(grid, row, "Resize the window to fit each image", cfg.fit_window_to_image);
    w.min_width = spin(cfg.min_window_width, 200, 8192, 10);
    add_row(grid, row, "Minimum window width", w.min_width);
    w.min_height = spin(cfg.min_window_height, 150, 8192, 10);
    add_row(grid, row, "Minimum window height", w.min_height);

    add_heading(grid, row, "Browsing");
    w.wrap_around = add_check(grid, row, "Wrap around at the end of the folder", cfg.wrap_around);
    w.show_hidden = add_check(grid, row, "Include hidden files", cfg.show_hidden);

    add_heading(grid, row, "Camera RAW");
    w.raw_mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.raw_mode), "full", "Full size (slow)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.raw_mode), "half", "Half size (fast)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.raw_mode), "preview", "Embedded JPEG preview (fastest)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.raw_mode), cfg.raw_mode.c_str());
    add_row(grid, row, "Decode", w.raw_mode);
    w.apply_xmp = add_check(grid, row, "Apply edits from XMP sidecars (exposure, crop, rotation)", cfg.apply_xmp_edits);

    add_heading(grid, row, "Maps");
    w.map_provider = gtk_combo_box_text_new();
    for (const auto& m : kMapProviders) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.map_provider), m.url, m.name);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.map_provider), "", "Custom");
    add_row(grid, row, "Open locations in", w.map_provider);
    w.map_url = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(w.map_url), 44);
    gtk_entry_set_text(GTK_ENTRY(w.map_url), cfg.map_url.c_str());
    gtk_widget_set_tooltip_text(w.map_url, "URL template; {lat} and {lon} are replaced with the coordinates");
    add_row(grid, row, "URL template", w.map_url);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.map_provider), cfg.map_url.c_str()))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.map_provider), "");
    g_signal_connect(w.map_provider, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer entry) {
                         const gchar* id = gtk_combo_box_get_active_id(combo);
                         if (id && *id) gtk_entry_set_text(GTK_ENTRY(entry), id);
                         gtk_widget_set_sensitive(GTK_WIDGET(entry), !id || !*id);
                     }), w.map_url);
    gtk_widget_set_sensitive(w.map_url, gtk_combo_box_get_active_id(GTK_COMBO_BOX(w.map_provider))[0] == '\0');
    return grid;
}

GtkWidget* build_viewing(const Config& cfg, Widgets& w) {
    GtkWidget* grid = page_grid();
    int row = 0;
    add_heading(grid, row, "Scaling");
    w.smooth_upscale = add_check(grid, row, "Smooth when zoomed in", cfg.smooth_upscale);
    w.smooth_downscale = add_check(grid, row, "Smooth when zoomed out", cfg.smooth_downscale);
    w.fullscreen_upscale = add_check(grid, row, "In fullscreen, enlarge small images to fill the screen", cfg.fullscreen_upscale);

    add_heading(grid, row, "Background");
    w.checkerboard = add_check(grid, row, "Show transparency as a checkerboard", cfg.checkerboard);
    w.background = color_button(cfg.background);
    add_row(grid, row, "Window", w.background);
    w.fullscreen_background = color_button(cfg.fullscreen_background);
    add_row(grid, row, "Fullscreen", w.fullscreen_background);
    return grid;
}

GtkWidget* build_file_types(const Config& cfg, Widgets& w) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    GtkWidget* label = gtk_label_new("File extensions to include when browsing a folder, separated by spaces:");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroller), GTK_SHADOW_IN);
    gtk_widget_set_vexpand(scroller, TRUE);
    w.extensions = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w.extensions), GTK_WRAP_WORD);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(w.extensions), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w.extensions), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(w.extensions), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w.extensions), 6);
    set_text_view(w.extensions, join_extensions(cfg.extensions));
    gtk_container_add(GTK_CONTAINER(scroller), w.extensions);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);

    GtkWidget* reset = gtk_button_new_with_label("Reset to all supported formats");
    g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer view) {
                         set_text_view(static_cast<GtkWidget*>(view), join_extensions(default_extensions()));
                     }), w.extensions);

    // Registers with the desktop (per user, no privileges) for the types listed above.
    GtkWidget* defaults = gtk_button_new_with_label("Make IfritView the default viewer for these types");
    GtkWidget* result = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(result), TRUE);
    gtk_label_set_xalign(GTK_LABEL(result), 0.0f);
    gtk_widget_set_halign(result, GTK_ALIGN_START);
    struct DefaultsCtx { GtkWidget* view; GtkWidget* result; };
    auto on_defaults = +[](GtkWidget*, gpointer p) {
        auto* ctx = static_cast<DefaultsCtx*>(p);
        std::vector<std::string> skipped;
        std::string error;
        const int n = set_default_for_extensions(parse_extensions(get_text_view(ctx->view)), skipped, error);
        std::string msg = n ? "IfritView is now the default for " + std::to_string(n) + " file types." : "No file types were set.";
        if (!skipped.empty()) {
            msg += " Unknown to the system: ";
            for (size_t i = 0; i < skipped.size(); ++i) msg += (i ? ", " : "") + skipped[i];
            msg += ".";
        }
        if (!error.empty()) msg += " Error: " + error;
        gtk_label_set_text(GTK_LABEL(ctx->result), msg.c_str());
    };
    auto* ctx = new DefaultsCtx{w.extensions, result};
    g_object_set_data_full(G_OBJECT(defaults), "ctx", ctx, +[](gpointer p) { delete static_cast<DefaultsCtx*>(p); });
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults), ctx);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(buttons), defaults, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), result, FALSE, FALSE, 0);
    return box;
}

GtkWidget* build_controls(const Config& cfg, Widgets& w) {
    GtkWidget* grid = page_grid();
    int row = 0;
    std::string group;
    for (const ControlInfo& c : kControls) {
        // Group rows by button: "Left click", "Left drag"... share a heading.
        std::string g = c.kind == Kind::Wheel ? "Wheel" : std::string(c.label).substr(0, std::string(c.label).find(' ')) + " button";
        if (g != group) {
            add_heading(grid, row, g.c_str());
            group = g;
        }
        GtkWidget* combo = gtk_combo_box_text_new();
        for (const ActionInfo& a : kActions)
            if (action_fits(a.id, c.kind)) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), a.key, a.label);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), action_info(cfg.control(c.id)).key);
        add_row(grid, row, c.label, combo);
        w.controls[size_t(c.id)] = combo;
    }
    GtkWidget* reset = gtk_button_new_with_label("Reset to defaults");
    gtk_widget_set_halign(reset, GTK_ALIGN_END);
    gtk_widget_set_margin_top(reset, 12);
    using Combos = std::array<GtkWidget*, size_t(Control::Count)>;
    auto reset_cb = +[](GtkWidget*, gpointer p) {
        auto* combos = static_cast<Combos*>(p);
        const Bindings defaults = default_bindings();
        for (size_t i = 0; i < combos->size(); ++i)
            gtk_combo_box_set_active_id(GTK_COMBO_BOX((*combos)[i]), action_info(defaults[i]).key);
    };
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_cb), &w.controls);
    gtk_grid_attach(GTK_GRID(grid), reset, 0, row++, 2, 1);

    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), grid);
    return scroller;
}

void apply(const Widgets& w, Config& cfg) {
    cfg.fit_window_to_image = checked(w.fit_window);
    cfg.min_window_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w.min_width));
    cfg.min_window_height = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w.min_height));
    cfg.wrap_around = checked(w.wrap_around);
    cfg.show_hidden = checked(w.show_hidden);
    if (const gchar* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w.raw_mode))) cfg.raw_mode = id;
    cfg.apply_xmp_edits = checked(w.apply_xmp);
    cfg.smooth_upscale = checked(w.smooth_upscale);
    cfg.smooth_downscale = checked(w.smooth_downscale);
    cfg.fullscreen_upscale = checked(w.fullscreen_upscale);
    cfg.checkerboard = checked(w.checkerboard);
    cfg.background = color_hex(w.background);
    cfg.fullscreen_background = color_hex(w.fullscreen_background);
    cfg.extensions = parse_extensions(get_text_view(w.extensions));
    const std::string url = gtk_entry_get_text(GTK_ENTRY(w.map_url));
    if (url.find("{lat}") != std::string::npos && url.find("{lon}") != std::string::npos) cfg.map_url = url;
    for (const ControlInfo& c : kControls)
        if (const gchar* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w.controls[size_t(c.id)])))
            cfg.controls[size_t(c.id)] = action_from_key(id, Action::None);
}

}  // namespace

bool run_settings_dialog(GtkWindow* parent, Config& cfg, const char* page) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Settings", parent,
                                                    GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                                    "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 660, 560);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* categories = gtk_list_box_new();
    gtk_widget_set_size_request(categories, 160, -1);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(categories), GTK_SELECTION_BROWSE);
    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(stack, TRUE);

    Widgets w{};
    struct Page {
        const char* id;
        const char* title;
        GtkWidget* widget;
    };
    const Page pages[] = {
        {"general", "General", build_general(cfg, w)},
        {"viewing", "Viewing", build_viewing(cfg, w)},
        {"controls", "Controls", build_controls(cfg, w)},
        {"files", "File Types", build_file_types(cfg, w)},
    };
    for (const Page& p : pages) {
        GtkWidget* label = gtk_label_new(p.title);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        gtk_widget_set_margin_top(label, 8);
        gtk_widget_set_margin_bottom(label, 8);
        GtkWidget* row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), label);
        g_object_set_data(G_OBJECT(row), "page", const_cast<char*>(p.id));
        gtk_container_add(GTK_CONTAINER(categories), row);
        gtk_stack_add_named(GTK_STACK(stack), p.widget, p.id);
    }
    g_signal_connect(categories, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer stack) {
                         if (row) gtk_stack_set_visible_child_name(GTK_STACK(stack), static_cast<const char*>(g_object_get_data(G_OBJECT(row), "page")));
                     }), stack);
    int initial = 0;
    for (size_t i = 0; page && i < std::size(pages); ++i)
        if (std::string(pages[i].id) == page) initial = int(i);
    gtk_list_box_select_row(GTK_LIST_BOX(categories), gtk_list_box_get_row_at_index(GTK_LIST_BOX(categories), initial));

    gtk_box_pack_start(GTK_BOX(hbox), categories, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), stack, TRUE, TRUE, 0);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);

    const bool ok = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
    if (ok) apply(w, cfg);
    gtk_widget_destroy(dialog);
    return ok;
}
