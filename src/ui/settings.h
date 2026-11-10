#pragma once

#include <rapport/rapport.h>

#include <functional>

struct Config;

// The settings dialog: General, Viewing, Controls and File Types tabs.
// Non-modal in code (rapport dialogs are callback-driven): `done(true)` runs
// after `cfg` has been updated from an OK; `done(false)` on Cancel.
// `page`: the tab to open on (0 General, 1 Viewing, 2 Controls, 3 File Types,
// 4 About).
// `forget_locations`: the Maps group's button, run at once rather than on
// OK; it calls back on the UI thread, told whether all was forgotten.
// Returns a function that ends the dialog as OK (true) or Cancel would; it
// does nothing once the dialog has gone.
using ForgetLocations = std::function<void(std::function<void(bool forgotten)> done)>;
std::function<void(bool ok)> open_settings_dialog(rap::ui& ui, rap::window* parent, Config& cfg, std::function<void(bool ok)> done,
                                                  ForgetLocations forget_locations, int page = 0);
