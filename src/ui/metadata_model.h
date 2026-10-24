#pragma once

#include "i18n.h"

#include "image.h"

#include <rapport/rapport.h>

#include <string>
#include <vector>

// The metadata pane's rows: each group as a bold heading, then its tags.
// A photo with GPS gets a "Location" row first in its group; activating it
// opens the map link.
class MetadataModel : public rap::list_model
{
public:
    void set(const std::vector<MetadataGroup>& groups, const Image* image, const std::string& map_url)
    {
        rows_.clear();
        for (const MetadataGroup& g : groups)
        {
            if (!rows_.empty())
                rows_.push_back({"", "", Kind::Gap, ""});
            // Names are kept in English (they're looked up by them) and
            // shown in the user's language where there's a translation.
            rows_.push_back({tr_ifrit(g.name.c_str()), "", Kind::Heading, ""});
            if (g.name == "GPS" && image && image->has_location)
                rows_.push_back({_("Location"), image->location_text(), Kind::Link, map_url});
            for (const MetadataItem& item : g.items)
                rows_.push_back({tr_ifrit(item.key.c_str()), item.value, Kind::Item, ""});
        }
        notify_reset();
    }

    // The map link of a row, if it has one.
    std::string url(size_t row) const { return row < rows_.size() ? rows_[row].url : std::string(); }

    size_t row_count() const override { return rows_.size(); }
    std::string text(size_t row, size_t column) const override
    {
        if (row >= rows_.size())
            return {};
        return column == 0 ? rows_[row].key : rows_[row].value;
    }
    rap::text_style style(size_t row, size_t column) const override
    {
        rap::text_style s;
        if (row >= rows_.size())
            return s;
        if (rows_[row].kind == Kind::Heading)
            s.bold = true;
        if (rows_[row].kind == Kind::Link && column == 1)
            s.fg = rap::paint{rap::theme_color::link};
        return s;
    }

private:
    enum class Kind
    {
        Heading,
        Item,
        Link,
        Gap
    };
    struct Row
    {
        std::string key, value;
        Kind kind;
        std::string url;
    };
    std::vector<Row> rows_;
};
