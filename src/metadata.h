#pragma once

#include "image.h"

#include <gtk/gtk.h>

#include <string>
#include <vector>

// A two-column list of everything known about the current image, in a
// small window that floats above the main one and follows image changes.
class MetadataWindow
{
public:
    explicit MetadataWindow(GtkWindow* parent);
    void toggle();
    bool visible() const;
    // `groups` are shown in order with a heading each; the optional location
    // becomes a clickable map link.
    void set(const std::string& title, const std::vector<MetadataGroup>& groups, const Image* image, const std::string& map_url);

private:
    GtkWidget* window_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkListStore* store_ = nullptr;
};
