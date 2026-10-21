#pragma once

#include <gtk/gtk.h>

struct Config;

// Modal settings dialog. Returns true and updates `cfg` when the user confirms.
bool run_settings_dialog(GtkWindow* parent, Config& cfg, const char* page = nullptr);
