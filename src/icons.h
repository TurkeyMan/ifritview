#pragma once

#include <gtk/gtk.h>

// Toolbar icons without the startup cost. Rendering a symbolic SVG through
// librsvg costs ~20 ms each; fifteen of them is most of the time between
// launch and the first frame. Rendered icons are kept as PNGs under the
// user's cache directory, keyed by icon theme, scale factor and foreground
// colour, and loaded from there on later runs.

// The widget whose theme and colour key the cache; set before any lookups.
void set_icon_reference(GtkWidget* widget);

// A GtkImage for `name` at `size` logical pixels. Uses the cache when it
// has the icon, otherwise the icon theme (and schedules caching).
GtkWidget* cached_icon_image(const char* name, int size);

// Called once after the first frame: renders and stores any icons that were
// missing from the cache. Cheap when nothing is missing.
void flush_icon_cache(GtkWidget* reference);
