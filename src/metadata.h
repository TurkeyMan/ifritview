#pragma once

#include "image.h"

#include <gtk/gtk.h>

#include <string>
#include <vector>

// A two-column list of everything known about the current image, docked
// as a pane to the right of the image (a separate window can't be placed
// sensibly on Wayland). Follows image changes while shown.
class MetadataWindow
{
public:
    explicit MetadataWindow(GtkWindow* parent);
    GtkWidget* widget() const { return pane_; }
    void toggle();
    bool visible() const;
    int width() const { return 380; }  // logical pixels; the window grows by this to make room
    // `groups` are shown in order with a heading each; the optional location
    // becomes a clickable map link.
    void set(const std::string& title, const std::vector<MetadataGroup>& groups, const Image* image, const std::string& map_url);

private:
    GtkWidget* pane_ = nullptr;
    GtkWidget* title_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkListStore* store_ = nullptr;
};
