// Fitting stored metadata to the file being saved, and leaving out the
// groups the user chose to strip.
//
// EXIF is edited in place: values are rewritten where they are, a removed
// entry is taken out of its directory and its data zeroed, and nothing else
// moves, so offsets stay valid (maker notes keep internal ones). Some
// writers share bytes between entries, so data is only zeroed at the end,
// where nothing kept still points. XMP is
// parsed into a small XML tree, edited by namespace URI and written back.
// IPTC is a list of datasets, filtered.
#include "file_metadata.h"

#include "../image.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace
{

// -- EXIF ---------------------------------------------------------------------

// Which group a tag belongs to; `always` ones describe the file and are kept
// whatever the choice (the editor fixes them up instead).
enum class Group
{
    always,
    location,
    personal,
    author,
    description,
    capture,
    history
};

constexpr uint16_t kExifIfd = 0x8769, kGpsIfd = 0x8825, kInteropIfd = 0xA005;

Group ifd0_group(uint16_t tag)
{
    switch (tag)
    {
        case 0x010E:  // ImageDescription
        case 0x9C9B:  // XPTitle
        case 0x9C9C:  // XPComment
        case 0x9C9E:  // XPKeywords
        case 0x9C9F:  // XPSubject
        case 0x4746:  // Rating
        case 0x4749:  // RatingPercent
            return Group::description;
        case 0x010F:  // Make
        case 0x0110:  // Model
            return Group::capture;
        case 0x000B:  // ProcessingSoftware
        case 0x0131:  // Software
        case 0x0132:  // DateTime (of the last change)
        case 0x013C:  // HostComputer
            return Group::history;
        case 0x013B:  // Artist
        case 0x8298:  // Copyright
        case 0x9C9D:  // XPAuthor
            return Group::author;
        case kGpsIfd:
            return Group::location;
        default:
            return Group::always;  // orientation, resolution, YCbCr positioning, the Exif IFD...
    }
}

Group exif_group(uint16_t tag)
{
    switch (tag)
    {
        case 0x9000:  // ExifVersion
        case 0x9101:  // ComponentsConfiguration
        case 0xA000:  // FlashpixVersion
        case 0xA001:  // ColorSpace
        case 0xA002:  // PixelXDimension
        case 0xA003:  // PixelYDimension
        case kInteropIfd:
            return Group::always;
        case 0x927C:  // MakerNote: full of serial numbers, among much else
        case 0xA420:  // ImageUniqueID
        case 0xA430:  // CameraOwnerName
        case 0xA431:  // BodySerialNumber
        case 0xA435:  // LensSerialNumber
            return Group::personal;
        case 0x9286:  // UserComment
            return Group::description;
        default:
            return Group::capture;  // exposure, lens, flash, dates...
    }
}

bool kept(Group g, const MetadataChoice& c)
{
    switch (g)
    {
        case Group::always: return true;
        case Group::location: return c.location;
        case Group::personal: return c.personal;
        case Group::author: return c.author;
        case Group::description: return c.description;
        case Group::capture: return c.capture;
        case Group::history: return c.history;
    }
    return true;
}

class TiffEditor
{
public:
    explicit TiffEditor(std::vector<uint8_t> bytes) : b_(std::move(bytes)), le_(b_.size() >= 2 && b_[0] == 'I') {}

    bool valid() const { return b_.size() >= 8 && u16(2) == 42; }
    uint32_t ifd0() const { return u32(4); }

    uint16_t u16(size_t p) const { return p + 2 <= b_.size() ? uint16_t(le_ ? b_[p] | b_[p + 1] << 8 : b_[p] << 8 | b_[p + 1]) : 0; }
    uint32_t u32(size_t p) const
    {
        if (p + 4 > b_.size())
            return 0;
        return le_ ? uint32_t(b_[p]) | uint32_t(b_[p + 1]) << 8 | uint32_t(b_[p + 2]) << 16 | uint32_t(b_[p + 3]) << 24
                   : uint32_t(b_[p]) << 24 | uint32_t(b_[p + 1]) << 16 | uint32_t(b_[p + 2]) << 8 | b_[p + 3];
    }
    void put16(size_t p, uint16_t v)
    {
        if (p + 2 > b_.size())
            return;
        b_[p + (le_ ? 0 : 1)] = uint8_t(v);
        b_[p + (le_ ? 1 : 0)] = uint8_t(v >> 8);
    }
    void put32(size_t p, uint32_t v)
    {
        if (p + 4 > b_.size())
            return;
        for (int i = 0; i < 4; ++i)
            b_[p + (le_ ? i : 3 - i)] = uint8_t(v >> (8 * i));
    }

    // An IFD that fits in the buffer: its entry count, else 0.
    uint16_t entries(uint32_t ifd) const
    {
        if (ifd < 8 || size_t(ifd) + 2 > b_.size())
            return 0;
        const uint16_t n = u16(ifd);
        return size_t(ifd) + 2 + size_t(n) * 12 + 4 <= b_.size() ? n : 0;
    }
    size_t entry(uint32_t ifd, int i) const { return size_t(ifd) + 2 + size_t(i) * 12; }
    uint32_t next(uint32_t ifd) const { return entries(ifd) ? u32(entry(ifd, entries(ifd))) : 0; }

    // The entry for `tag`, or 0.
    size_t find(uint32_t ifd, uint16_t tag) const
    {
        for (int i = 0; i < entries(ifd); ++i)
            if (u16(entry(ifd, i)) == tag)
                return entry(ifd, i);
        return 0;
    }

    static size_t type_size(uint16_t type)
    {
        switch (type)
        {
            case 1: case 2: case 6: case 7: return 1;
            case 3: case 8: return 2;
            case 4: case 9: case 11: case 13: return 4;
            case 5: case 10: case 12: return 8;
        }
        return 0;
    }

    // The bytes an entry's value occupies outside the IFD, if it does.
    bool external(size_t e, size_t& offset, size_t& length) const
    {
        const uint64_t len = uint64_t(type_size(u16(e + 2))) * u32(e + 4);
        if (len <= 4)
            return false;
        offset = u32(e + 8);
        length = size_t(len);
        return offset >= 8 && offset <= b_.size() && length <= b_.size() - offset;
    }

    // Marks bytes to clear once editing is done (see finish).
    void zero(size_t offset, size_t length)
    {
        if (offset >= b_.size())
            return;
        length = std::min(length, b_.size() - offset);
        zeroed_.push_back({offset, offset + length});
    }

    // Zeroes an IFD, everything its entries point to, and its sub-IFDs.
    void wipe_ifd(uint32_t ifd, int depth = 0)
    {
        const uint16_t n = entries(ifd);
        if (!n || depth > 4)
            return;
        for (int i = 0; i < n; ++i)
            wipe_value(entry(ifd, i), depth);
        zero(ifd, 2 + size_t(n) * 12 + 4);
    }

    void wipe_value(size_t e, int depth)
    {
        const uint16_t tag = u16(e);
        if (tag == kExifIfd || tag == kGpsIfd || tag == kInteropIfd)
            wipe_ifd(u32(e + 8), depth + 1);
        size_t offset, length;
        if (external(e, offset, length))
            zero(offset, length);
    }

    // Takes an entry out of its IFD: the rest move up, the freed slot and the
    // value are zeroed.
    void remove(uint32_t ifd, size_t e)
    {
        wipe_value(e, 0);
        const uint16_t n = entries(ifd);
        const size_t end = entry(ifd, n) + 4;  // past the next-IFD offset
        std::memmove(&b_[e], &b_[e + 12], end - e - 12);
        std::fill_n(b_.begin() + (end - 12), 12, 0);
        put16(ifd, uint16_t(n - 1));
    }

    // Removes, from `ifd`, the entries `keep` says no to.
    void filter(uint32_t ifd, const std::function<bool(uint16_t)>& keep)
    {
        for (int i = entries(ifd) - 1; i >= 0; --i)
        {
            const size_t e = entry(ifd, i);
            if (!keep(u16(e)))
                remove(ifd, e);
        }
    }

    // A SHORT or LONG value in place (made a LONG when it won't fit a SHORT).
    // Some writers give a count of 2 SHORTs: the first is set, the other 0.
    void set_number(size_t e, uint32_t v)
    {
        if (!e)
            return;
        const uint32_t count = u32(e + 4);
        if (u16(e + 2) == 3 && count == 2 && v <= 0xFFFF)
        {
            put16(e + 8, uint16_t(v));
            put16(e + 10, 0);
            return;
        }
        if (count != 1)
            return;
        if (u16(e + 2) == 3 && v <= 0xFFFF)
        {
            put16(e + 8, uint16_t(v));
            put16(e + 10, 0);
        }
        else
        {
            put16(e + 2, 4);
            put32(e + 8, v);
        }
    }

    // Drops IFD1 (the thumbnail) and the image it points to.
    void drop_thumbnail()
    {
        const uint32_t ifd1 = next(ifd0());
        if (!ifd1)
            return;
        if (const size_t at = find(ifd1, 0x0201))
            if (const size_t len = find(ifd1, 0x0202))
                zero(u32(at + 8), u32(len + 8));
        wipe_ifd(ifd1);
        put32(entry(ifd0(), entries(ifd0())), 0);
    }

    // The bytes, with what was let go zeroed (save what a kept entry still
    // uses) and cut off where that reaches the end.
    std::vector<uint8_t> finish()
    {
        std::vector<bool> live(b_.size(), false);
        const auto mark = [&](size_t from, size_t length)
        {
            for (size_t i = from; i < std::min(b_.size(), from + length); ++i)
                live[i] = true;
        };
        mark(0, 8);
        std::function<void(uint32_t, int)> walk = [&](uint32_t ifd, int depth)
        {
            const uint16_t n = entries(ifd);
            if (!n || depth > 4)
                return;
            mark(ifd, 2 + size_t(n) * 12 + 4);
            for (int i = 0; i < n; ++i)
            {
                const size_t e = entry(ifd, i);
                const uint16_t tag = u16(e);
                size_t offset, length;
                if (external(e, offset, length))
                    mark(offset, length);
                if (tag == kExifIfd || tag == kGpsIfd || tag == kInteropIfd)
                    walk(u32(e + 8), depth + 1);
            }
            if (const size_t at = find(ifd, 0x0201))  // a thumbnail's JPEG, which the entry's own size doesn't cover
                if (const size_t len = find(ifd, 0x0202))
                    mark(u32(at + 8), u32(len + 8));
        };
        walk(ifd0(), 0);
        if (const uint32_t ifd1 = next(ifd0()))
            walk(ifd1, 0);
        size_t end = b_.size();
        std::sort(zeroed_.begin(), zeroed_.end());
        for (const auto& [from, to] : zeroed_)
            for (size_t i = from; i < to; ++i)
                if (!live[i])
                    b_[i] = 0;
        // Trailing bytes nothing uses any more go.
        while (end > 8 && !live[end - 1] && std::any_of(zeroed_.begin(), zeroed_.end(), [&](const auto& r) { return r.first < end && end <= r.second; }))
            --end;
        b_.resize(end);
        return std::move(b_);
    }

private:
    std::vector<uint8_t> b_;
    bool le_;
    std::vector<std::pair<size_t, size_t>> zeroed_;
};

// -- XMP ----------------------------------------------------------------------

// Just enough XML for XMP packets: elements, attributes and everything else
// (text, comments, processing instructions, CDATA) kept as written.
struct Node
{
    enum Kind
    {
        element,
        raw
    };
    Kind kind = raw;
    std::string name;  // qualified, as written
    std::vector<std::pair<std::string, std::string>> attrs;  // values as written (escaped)
    std::vector<Node> children;
    std::string text;  // raw nodes
    bool self_closing = false;
};

class XmlParser
{
public:
    explicit XmlParser(const std::string& s) : s_(s) {}

    bool parse(std::vector<Node>& out)
    {
        while (p_ < s_.size())
        {
            Node n;
            if (!node(n))
                return false;
            out.push_back(std::move(n));
        }
        return true;
    }

private:
    bool node(Node& n)
    {
        if (s_[p_] != '<')
        {
            const size_t end = s_.find('<', p_);
            n.text = s_.substr(p_, end == std::string::npos ? std::string::npos : end - p_);
            p_ = end == std::string::npos ? s_.size() : end;
            return true;
        }
        for (const char* open : {"<!--", "<![CDATA[", "<?", "<!"})
            if (s_.compare(p_, std::strlen(open), open) == 0)
            {
                const char* close = open[1] == '!' && open[2] == '-' ? "-->" : open[2] == '[' ? "]]>" : open[1] == '?' ? "?>" : ">";
                const size_t end = s_.find(close, p_);
                if (end == std::string::npos)
                    return false;
                n.text = s_.substr(p_, end + std::strlen(close) - p_);
                p_ = end + std::strlen(close);
                return true;
            }
        if (s_.compare(p_, 2, "</") == 0)
            return false;  // a stray end tag
        n.kind = Node::element;
        ++p_;
        n.name = name();
        if (n.name.empty())
            return false;
        for (;;)
        {
            space();
            if (p_ >= s_.size())
                return false;
            if (s_.compare(p_, 2, "/>") == 0)
            {
                p_ += 2;
                n.self_closing = true;
                return true;
            }
            if (s_[p_] == '>')
            {
                ++p_;
                break;
            }
            std::string key = name();
            space();
            if (key.empty() || p_ >= s_.size() || s_[p_] != '=')
                return false;
            ++p_;
            space();
            if (p_ >= s_.size() || (s_[p_] != '"' && s_[p_] != '\''))
                return false;
            const char q = s_[p_++];
            const size_t end = s_.find(q, p_);
            if (end == std::string::npos)
                return false;
            n.attrs.emplace_back(std::move(key), s_.substr(p_, end - p_));
            p_ = end + 1;
        }
        while (p_ < s_.size() && s_.compare(p_, 2, "</") != 0)
        {
            Node child;
            if (!node(child))
                return false;
            n.children.push_back(std::move(child));
        }
        if (p_ >= s_.size())
            return false;
        p_ += 2;
        if (name() != n.name)
            return false;
        space();
        if (p_ >= s_.size() || s_[p_] != '>')
            return false;
        ++p_;
        return true;
    }

    std::string name()
    {
        const size_t start = p_;
        while (p_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[p_])) && s_[p_] != '=' && s_[p_] != '>' && s_[p_] != '/')
            ++p_;
        return s_.substr(start, p_ - start);
    }
    void space()
    {
        while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_])))
            ++p_;
    }

    const std::string& s_;
    size_t p_ = 0;
};

void write(const Node& n, std::string& out)
{
    if (n.kind == Node::raw)
    {
        out += n.text;
        return;
    }
    out += '<' + n.name;
    for (const auto& [k, v] : n.attrs)
        out += ' ' + k + "=\"" + v + '"';
    if (n.self_closing)
    {
        out += "/>";
        return;
    }
    out += '>';
    for (const Node& c : n.children)
        write(c, out);
    out += "</" + n.name + '>';
}

namespace ns
{
const std::string rdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
const std::string exif = "http://ns.adobe.com/exif/1.0/";
const std::string exif_ex = "http://cipa.jp/exif/1.0/";
const std::string tiff = "http://ns.adobe.com/tiff/1.0/";
const std::string aux = "http://ns.adobe.com/exif/1.0/aux/";
const std::string dc = "http://purl.org/dc/elements/1.1/";
const std::string xmp = "http://ns.adobe.com/xap/1.0/";
const std::string xmp_mm = "http://ns.adobe.com/xap/1.0/mm/";
const std::string xmp_rights = "http://ns.adobe.com/xap/1.0/rights/";
const std::string photoshop = "http://ns.adobe.com/photoshop/1.0/";
const std::string crs = "http://ns.adobe.com/camera-raw-settings/1.0/";
const std::string iptc_core = "http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/";
const std::string iptc_ext = "http://iptc.org/std/Iptc4xmpExt/2008-02-29/";
const std::string lr = "http://ns.adobe.com/lightroom/1.0/";
const std::string mwg_regions = "http://www.metadataworkinggroup.com/schemas/regions/";
const std::string ms_photo = "http://ns.microsoft.com/photo/1.0/";
const std::string ms_photo_12 = "http://ns.microsoft.com/photo/1.2/";
const std::string plus = "http://ns.useplus.org/ldf/xmp/1.0/";
}  // namespace ns

// The group of a top-level XMP property, by namespace and local name.
Group xmp_group(const std::string& uri, const std::string& local)
{
    using namespace ns;
    const auto is = [&](const std::string& u, std::initializer_list<const char*> names)
    {
        if (uri != u)
            return false;
        for (const char* n : names)
            if (local == n)
                return true;
        return false;
    };
    if ((uri == exif && local.rfind("GPS", 0) == 0) || is(photoshop, {"City", "State", "Country"}) || is(iptc_core, {"Location", "CountryCode"}) ||
        is(iptc_ext, {"LocationCreated", "LocationShown"}))
        return Group::location;
    if (is(aux, {"SerialNumber", "LensSerialNumber", "OwnerName"}) || is(exif_ex, {"BodySerialNumber", "LensSerialNumber", "CameraOwnerName"}) ||
        is(exif, {"ImageUniqueID"}) || is(iptc_ext, {"PersonInImage"}) || uri == mwg_regions || is(ms_photo_12, {"RegionInfo"}))
        return Group::personal;
    if (is(dc, {"creator", "rights", "publisher", "contributor"}) || uri == xmp_rights || uri == plus ||
        is(photoshop, {"AuthorsPosition", "Credit", "Source"}) || is(iptc_core, {"CreatorContactInfo"}))
        return Group::author;
    if (is(dc, {"title", "description", "subject"}) || is(xmp, {"Rating", "Label"}) || uri == lr || uri == ms_photo ||
        is(photoshop, {"Headline", "CaptionWriter", "Instructions", "Category", "SupplementalCategories", "TransmissionReference", "Urgency"}) ||
        is(iptc_core, {"IntellectualGenre", "Scene", "SubjectCode"}) || is(iptc_ext, {"Event"}))
        return Group::description;
    if (is(tiff, {"Orientation", "ImageWidth", "ImageLength"}) || is(exif, {"PixelXDimension", "PixelYDimension", "ColorSpace"}) ||
        is(photoshop, {"ColorMode", "ICCProfile"}))
        return Group::always;
    if (uri == exif || uri == exif_ex || uri == tiff || uri == aux || is(xmp, {"CreateDate"}) || is(photoshop, {"DateCreated"}))
        return Group::capture;
    if (is(xmp, {"CreatorTool", "ModifyDate", "MetadataDate"}) || uri == xmp_mm || uri == crs || is(photoshop, {"History", "DocumentAncestors"}))
        return Group::history;
    return Group::always;  // unknown schemas: kept with the file
}

using Prefixes = std::map<std::string, std::string>;  // prefix -> URI

void declare(const Node& n, Prefixes& p)
{
    for (const auto& [k, v] : n.attrs)
        if (k.rfind("xmlns:", 0) == 0)
            p[k.substr(6)] = v;
}

bool resolve(const std::string& qname, const Prefixes& p, std::string& uri, std::string& local)
{
    const size_t colon = qname.find(':');
    if (colon == std::string::npos)
        return false;
    const auto it = p.find(qname.substr(0, colon));
    if (it == p.end())
        return false;
    uri = it->second;
    local = qname.substr(colon + 1);
    return true;
}

struct XmpEdit
{
    const MetadataChoice& choice;
    const SaveTarget& target;
    int removed = 0;

    // A fixed value for a property that describes the file, if it's one.
    bool fixed(const std::string& uri, const std::string& local, std::string& value) const
    {
        if (uri == ns::tiff && local == "Orientation")
            value = "1";
        else if (target.width > 0 && ((uri == ns::exif && local == "PixelXDimension") || (uri == ns::tiff && local == "ImageWidth")))
            value = std::to_string(target.width);
        else if (target.height > 0 && ((uri == ns::exif && local == "PixelYDimension") || (uri == ns::tiff && local == "ImageLength")))
            value = std::to_string(target.height);
        else
            return false;
        return true;
    }

    bool keep(const std::string& uri, const std::string& local) const
    {
        if (target.drop_develop && uri == ns::crs)
            return false;
        return kept(xmp_group(uri, local), choice);
    }

    // Edits the properties of an rdf:Description (attributes and children).
    void description(Node& d, Prefixes p)
    {
        declare(d, p);
        std::string uri, local, value;
        auto& attrs = d.attrs;
        for (auto it = attrs.begin(); it != attrs.end();)
        {
            if (it->first.rfind("xmlns", 0) != 0 && resolve(it->first, p, uri, local) && uri != ns::rdf)
            {
                if (fixed(uri, local, value))
                    it->second = value;
                else if (!keep(uri, local))
                {
                    it = attrs.erase(it);
                    ++removed;
                    continue;
                }
            }
            ++it;
        }
        auto& kids = d.children;
        for (auto it = kids.begin(); it != kids.end();)
        {
            Prefixes inner = p;
            declare(*it, inner);
            if (it->kind == Node::element && resolve(it->name, inner, uri, local) && uri != ns::rdf)
            {
                if (fixed(uri, local, value) && it->children.size() <= 1 && (it->children.empty() || it->children[0].kind == Node::raw))
                {
                    it->children.assign(1, Node{});
                    it->children[0].text = value;
                }
                else if (!keep(uri, local))
                {
                    it = kids.erase(it);
                    // The whitespace that led up to it goes too.
                    if (it != kids.begin() && std::prev(it)->kind == Node::raw &&
                        std::prev(it)->text.find_first_not_of(" \t\r\n") == std::string::npos)
                        it = kids.erase(std::prev(it));
                    ++removed;
                    continue;
                }
            }
            ++it;
        }
    }

    // Finds the rdf:Description elements under rdf:RDF.
    void walk(Node& n, Prefixes p)
    {
        if (n.kind != Node::element)
            return;
        declare(n, p);
        std::string uri, local;
        const bool rdf_description = resolve(n.name, p, uri, local) && uri == ns::rdf && local == "Description";
        if (rdf_description)
        {
            description(n, p);
            return;
        }
        for (Node& c : n.children)
            walk(c, p);
    }
};

// Notes the group of every top-level property, without changing anything.
void note_groups(const Node& n, Prefixes p, const std::function<void(Group)>& seen, bool in_description = false)
{
    if (n.kind != Node::element)
        return;
    declare(n, p);
    std::string uri, local;
    if (in_description)
    {
        if (resolve(n.name, p, uri, local) && uri != ns::rdf)
            seen(xmp_group(uri, local));
        return;
    }
    if (resolve(n.name, p, uri, local) && uri == ns::rdf && local == "Description")
    {
        for (const auto& [k, v] : n.attrs)
            if (k.rfind("xmlns", 0) != 0 && resolve(k, p, uri, local) && uri != ns::rdf)
                seen(xmp_group(uri, local));
        for (const Node& c : n.children)
            note_groups(c, p, seen, true);
        return;
    }
    for (const Node& c : n.children)
        note_groups(c, p, seen);
}

// -- IPTC ---------------------------------------------------------------------

Group iptc_group(uint8_t record, uint8_t dataset)
{
    if (record != 2)
        return Group::always;  // the envelope: character set and the like
    switch (dataset)
    {
        case 0:  // record version
            return Group::always;
        case 26: case 27: case 90: case 92: case 95: case 100: case 101:
            return Group::location;
        case 80: case 85: case 110: case 115: case 116: case 118:
            return Group::author;
        case 5: case 15: case 20: case 25: case 40: case 103: case 105: case 120: case 122:
            return Group::description;
        case 55: case 60: case 62: case 63:
            return Group::capture;
        case 65: case 70:
            return Group::history;
        default:
            return Group::description;
    }
}

}  // namespace

std::vector<uint8_t> edit_exif(const std::vector<uint8_t>& tiff, const MetadataChoice& choice, const SaveTarget& target)
{
    if (!choice.keep)
        return {};
    TiffEditor t(tiff);
    if (!t.valid() || !t.entries(t.ifd0()))
        return {};
    const uint32_t ifd0 = t.ifd0();
    // A thumbnail stored the camera's way round no longer matches once the
    // orientation says upright; nor does one of pixels since changed.
    const size_t orientation = t.find(ifd0, 0x0112);
    if (target.pixels_changed || (orientation && t.u16(orientation + 8) != 1))
        t.drop_thumbnail();
    t.set_number(t.find(ifd0, 0x0112), 1);  // the pixels are upright
    if (target.width > 0)
    {
        t.set_number(t.find(ifd0, 0x0100), uint32_t(target.width));
        t.set_number(t.find(ifd0, 0x0101), uint32_t(target.height));
    }
    if (const size_t e = t.find(ifd0, kExifIfd))
    {
        const uint32_t exif = t.u32(e + 8);
        if (target.width > 0)
        {
            t.set_number(t.find(exif, 0xA002), uint32_t(target.width));
            t.set_number(t.find(exif, 0xA003), uint32_t(target.height));
        }
        t.filter(exif, [&](uint16_t tag) { return kept(exif_group(tag), choice); });
    }
    t.filter(ifd0, [&](uint16_t tag) { return kept(ifd0_group(tag), choice); });
    return t.finish();
}

std::vector<uint8_t> edit_xmp(const std::vector<uint8_t>& packet, const MetadataChoice& choice, const SaveTarget& target)
{
    if (!choice.keep || packet.empty())
        return {};
    const std::string text(packet.begin(), packet.end());
    std::vector<Node> nodes;
    if (!XmlParser(text).parse(nodes))
        return {};  // not XML we can edit safely: left out rather than written unedited
    XmpEdit edit{choice, target};
    for (Node& n : nodes)
        edit.walk(n, {});
    std::string out;
    for (const Node& n : nodes)
        write(n, out);
    return std::vector<uint8_t>(out.begin(), out.end());
}

std::vector<uint8_t> edit_iptc(const std::vector<uint8_t>& iim, const MetadataChoice& choice)
{
    if (!choice.keep)
        return {};
    std::vector<uint8_t> out;
    bool content = false;
    size_t p = 0;
    while (p + 5 <= iim.size() && iim[p] == 0x1C)
    {
        const uint8_t record = iim[p + 1], dataset = iim[p + 2];
        size_t header = 5, len = size_t(iim[p + 3]) << 8 | iim[p + 4];
        if (len & 0x8000)
        {
            // Extended: the next (len & 0x7FFF) bytes are the length.
            const size_t n = len & 0x7FFF;
            if (n > 4 || p + 5 + n > iim.size())
                break;
            len = 0;
            for (size_t i = 0; i < n; ++i)
                len = len << 8 | iim[p + 5 + i];
            header += n;
        }
        if (len > iim.size() - p - header)
            break;
        const Group g = iptc_group(record, dataset);
        if (kept(g, choice))
        {
            out.insert(out.end(), iim.begin() + long(p), iim.begin() + long(p + header + len));
            content = content || (record == 2 && dataset != 0);
        }
        p += header + len;
    }
    return content ? out : std::vector<uint8_t>();
}

namespace
{

bool* group_flag(MetadataChoice& c, const std::string& name)
{
    if (name == "location") return &c.location;
    if (name == "personal") return &c.personal;
    if (name == "author") return &c.author;
    if (name == "description") return &c.description;
    if (name == "capture") return &c.capture;
    if (name == "history") return &c.history;
    return nullptr;
}

}  // namespace

bool parse_metadata_choice(const std::string& text, MetadataChoice& choice, std::string& error)
{
    MetadataChoice c;
    if (text == "all")
        ;
    else if (text == "none")
        c.keep = false;
    else if (text == "no-location")
        c.location = false;
    else if (text == "private")
        c.location = c.personal = false;
    else if (text == "copyright")
        c.location = c.personal = c.description = c.capture = c.history = false;
    else if (text.rfind("strip:", 0) == 0 || text.rfind("only:", 0) == 0)
    {
        const bool only = text[0] == 'o';
        if (only)
            c.location = c.personal = c.author = c.description = c.capture = c.history = false;
        std::string rest = text.substr(only ? 5 : 6);
        for (size_t start = 0; start <= rest.size();)
        {
            const size_t plus = rest.find('+', start);
            const std::string name = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            bool* flag = group_flag(c, name);
            if (!flag)
            {
                error = "unknown metadata group \"" + name + "\" (groups: location, personal, author, description, capture, history)";
                return false;
            }
            *flag = only;
            if (plus == std::string::npos)
                break;
            start = plus + 1;
        }
    }
    else
    {
        error = "unknown metadata choice \"" + text + "\" (all, no-location, private, copyright, none, strip:GROUPS or only:GROUPS)";
        return false;
    }
    choice = c;
    return true;
}

std::string metadata_preset(const MetadataChoice& c)
{
    for (const char* name : {"all", "no-location", "private", "copyright", "none"})
    {
        MetadataChoice p;
        std::string ignored;
        parse_metadata_choice(name, p, ignored);
        if (!c.keep && !p.keep)
            return name;
        if (c.keep && p.keep && c.location == p.location && c.personal == p.personal && c.author == p.author && c.description == p.description &&
            c.capture == p.capture && c.history == p.history)
            return name;
    }
    return "";
}

MetadataChoice metadata_present(const FileMetadata& m)
{
    MetadataChoice present;
    present.location = present.personal = present.author = present.description = present.capture = present.history = false;
    const auto seen = [&](Group g)
    {
        switch (g)
        {
            case Group::location: present.location = true; break;
            case Group::personal: present.personal = true; break;
            case Group::author: present.author = true; break;
            case Group::description: present.description = true; break;
            case Group::capture: present.capture = true; break;
            case Group::history: present.history = true; break;
            case Group::always: break;
        }
    };
    TiffEditor t(m.exif);
    if (t.valid())
    {
        const uint32_t ifd0 = t.ifd0();
        for (int i = 0; i < t.entries(ifd0); ++i)
        {
            const size_t e = t.entry(ifd0, i);
            const uint16_t tag = t.u16(e);
            if (tag == kGpsIfd)
            {
                // Some cameras write a GPS directory with only its version: no position.
                const uint32_t gps = t.u32(e + 8);
                for (int k = 0; k < t.entries(gps); ++k)
                    if (t.u16(t.entry(gps, k)) != 0)
                        present.location = true;
                continue;
            }
            seen(ifd0_group(tag));
        }
        if (const size_t e = t.find(ifd0, kExifIfd))
            for (int i = 0; i < t.entries(t.u32(e + 8)); ++i)
                seen(exif_group(t.u16(t.entry(t.u32(e + 8), i))));
    }
    if (!m.xmp.empty())
    {
        const std::string text(m.xmp.begin(), m.xmp.end());
        std::vector<Node> nodes;
        if (XmlParser(text).parse(nodes))
            for (const Node& n : nodes)
                note_groups(n, {}, seen);
    }
    for (size_t p = 0; p + 5 <= m.iptc.size() && m.iptc[p] == 0x1C;)
    {
        seen(iptc_group(m.iptc[p + 1], m.iptc[p + 2]));
        const size_t len = size_t(m.iptc[p + 3]) << 8 | m.iptc[p + 4];
        if (len & 0x8000)
            break;  // extended lengths are rare; enough has been seen
        p += 5 + len;
    }
    present.keep = !(m.exif.empty() && m.xmp.empty() && m.iptc.empty());
    return present;
}

std::vector<std::pair<uint16_t, std::string>> exif_strings(const std::vector<uint8_t>& tiff)
{
    std::vector<std::pair<uint16_t, std::string>> out;
    TiffEditor t(tiff);
    if (!t.valid())
        return out;
    const uint32_t ifd0 = t.ifd0();
    for (uint16_t tag : {0x010E, 0x010F, 0x0110, 0x0131, 0x0132, 0x013B, 0x8298})
    {
        const size_t e = t.find(ifd0, tag);
        if (!e || t.u16(e + 2) != 2)  // ASCII only
            continue;
        const size_t n = t.u32(e + 4);
        size_t offset = e + 8, length = n;
        if (n > 4 && !t.external(e, offset, length))
            continue;
        std::string s(reinterpret_cast<const char*>(tiff.data() + offset), length);
        s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
        if (!s.empty())
            out.emplace_back(tag, std::move(s));
    }
    return out;
}

std::string metadata_choice_text(const MetadataChoice& c)
{
    std::string text = metadata_preset(c);
    if (!text.empty())
        return text;
    text = "only:";
    for (const char* name : {"location", "personal", "author", "description", "capture", "history"})
    {
        MetadataChoice copy = c;
        if (*group_flag(copy, name))
            text += std::string(text.size() > 5 ? "+" : "") + name;
    }
    return text;
}

FileMetadata prepare_metadata(const FileMetadata& source, const MetadataChoice& choice, const SaveTarget& target)
{
    FileMetadata m;
    m.icc = source.icc;  // always: it's how the pixels are to be read
    if (!choice.keep)
        return m;
    if (!source.exif.empty())
        m.exif = edit_exif(source.exif, choice, target);
    if (!source.xmp.empty())
        m.xmp = edit_xmp(source.xmp, choice, target);
    if (!source.iptc.empty())
        m.iptc = edit_iptc(source.iptc, choice);
    return m;
}

std::shared_ptr<const FileMetadata> metadata_for_save(const Image& image, const MetadataChoice& choice, bool format_holds_metadata,
                                                      bool develop_applied, int width, int height)
{
    if (!image.stored)
        return nullptr;
    if (!format_holds_metadata)
    {
        if (image.stored->icc.empty())
            return nullptr;
        FileMetadata icc_only;  // part of what the pixels mean, whatever the format
        icc_only.icc = image.stored->icc;
        return std::make_shared<const FileMetadata>(std::move(icc_only));
    }
    SaveTarget target;
    target.width = width ? width : image.width;
    target.height = height ? height : image.height;
    target.pixels_changed = image.edited || image.exposure_edit != 0;
    target.drop_develop = develop_applied || image.is_raw;  // a raw's develop settings describe the raw, not this rendering
    return std::make_shared<const FileMetadata>(prepare_metadata(*image.stored, choice, target));
}
