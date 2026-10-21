#pragma once

#include <anyui/anyui.h>

#include <functional>

struct Config;

// The settings dialog: General, Viewing, Controls and File Types tabs.
// Non-modal in code (anyui dialogs are callback-driven): `done(true)` runs
// after `cfg` has been updated from an OK; `done(false)` on Cancel.
// `page`: the tab to open on (0 General, 1 Viewing, 2 Controls, 3 File Types).
void open_settings_dialog(anyui::Ui& ui, anyui::Window* parent, Config& cfg, std::function<void(bool ok)> done, int page = 0);
