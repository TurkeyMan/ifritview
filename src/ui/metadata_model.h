#pragma once

#include "image.h"

#include <anyui/anyui.h>

#include <string>
#include <vector>

// The metadata pane's rows: each group as a bold heading, then its tags.
// A photo with GPS gets a "Location" row first in its group; activating it
// opens the map link.
class MetadataModel : public anyui::ListModel
{
public:
    void set(const std::vector<MetadataGroup>& groups, const Image* image, const std::string& map_url)
    {
        rows_.clear();
        for (const MetadataGroup& g : groups)
        {
            if (!rows_.empty())
                rows_.push_back({"", "", Kind::Gap, ""});
            rows_.push_back({g.name, "", Kind::Heading, ""});
            if (g.name == "GPS" && image && image->has_location)
                rows_.push_back({"Location", image->location_text(), Kind::Link, map_url});
            for (const MetadataItem& item : g.items)
                rows_.push_back({item.key, item.value, Kind::Item, ""});
        }
        notifyReset();
    }

    // The map link of a row, if it has one.
    std::string url(size_t row) const { return row < rows_.size() ? rows_[row].url : std::string(); }

    size_t rowCount() const override { return rows_.size(); }
    std::string text(size_t row, size_t column) const override
    {
        if (row >= rows_.size())
            return {};
        return column == 0 ? rows_[row].key : rows_[row].value;
    }
    anyui::TextStyle style(size_t row, size_t column) const override
    {
        anyui::TextStyle s;
        if (row >= rows_.size())
            return s;
        if (rows_[row].kind == Kind::Heading)
            s.bold = true;
        if (rows_[row].kind == Kind::Link && column == 1)
            s.fg = anyui::Paint{anyui::ThemeColor::Link};
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
