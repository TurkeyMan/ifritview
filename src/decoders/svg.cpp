// SVG and SVGZ (gzipped SVG) through lunasvg, drawn once at the document's
// own size scaled up to at least kMinSide pixels on its longer side (most
// vectors are small), and at most kMaxSide.
#include "decoder.h"
#include "../i18n.h"

#include <lunasvg.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

constexpr double kMinSide = 2048, kMaxSide = 8192;

// Past a byte-order mark and white space.
const char* skip_space(const char* p, const char* end)
{
    if (end - p >= 3 && std::memcmp(p, "\xEF\xBB\xBF", 3) == 0)
        p += 3;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        ++p;
    return p;
}

bool starts(const char* p, const char* end, const char* s)
{
    const size_t n = std::strlen(s);
    return size_t(end - p) >= n && std::memcmp(p, s, n) == 0;
}

// The file's header is 64 bytes, often not enough to reach the root past an
// XML declaration: anything that may be an SVG; decode looks properly.
bool sniff(const uint8_t* h, size_t n)
{
    const char *p = skip_space(reinterpret_cast<const char*>(h), reinterpret_cast<const char*>(h) + n), *end = reinterpret_cast<const char*>(h) + n;
    return starts(p, end, "<svg") || starts(p, end, "<?xml") || starts(p, end, "<!--") || starts(p, end, "<!DOCTYPE svg") ||
           (n >= 2 && h[0] == 0x1F && h[1] == 0x8B);  // gzip: SVGZ, or not ours
}

// Whether the root element, past the XML declaration, comments and DOCTYPE, is <svg>.
bool svg_root(const std::string& text)
{
    const char *p = text.data(), *end = p + text.size();
    for (;;)
    {
        p = skip_space(p, end);
        const char* close = starts(p, end, "<?") ? "?>" : starts(p, end, "<!--") ? "-->" : starts(p, end, "<!") ? ">" : nullptr;
        if (!close)
            break;
        const char* q = std::search(p, end, close, close + std::strlen(close));
        if (*close == '>' && std::find(p, q, '[') != q)  // a DOCTYPE's internal subset
            q = std::search(p, end, "]>", "]>" + 2);
        if (q == end)
            return false;
        p = q + std::strlen(close);
    }
    return starts(p, end, "<svg") && end - p > 4 && std::strchr(" \t\r\n>/", p[4]);
}

// Whether lunasvg can build the document safely. lunasvg 3.5.0's parser
// (Document::parse) is followed here exactly as far as the tree goes (names,
// white space, quoting, entities, comments, CDATA, DOCTYPE, unknown elements
// skipped with what's in them), and its build simulated without drawing:
// each <use> gets a copy of what it points at as that is at the time,
// copies of copies included, as lunasvg makes them. lunasvg builds, lays
// out and draws recursively, so a tree thousands deep would overflow the
// stack, and <use>s of <use>s can multiply a small file into billions of
// elements; the simulation gives up past kMaxDepth deep or kMaxBytes (an
// estimate of what lunasvg would hold: each element, its attributes and text).
constexpr int kMaxDepth = 256;
constexpr size_t kMaxBytes = size_t(128) << 20, kElementBytes = 256, kTextBytes = 64;

class Model
{
public:
    // False if lunasvg wouldn't read it either, or it's past the limits.
    bool parse(std::string_view in);
    bool build() { return build(0, 1); }

private:
    enum Flag : uint8_t
    {
        kUse = 1,
        kClonable = 2,  // what a <use> may copy (lunasvg's isDisallowedElement)
        kText = 4       // keeps its text (text, tspan, style)
    };
    struct Node
    {
        int parent = -1, id = -1, href = -1;  // id and href (the part after '#') interned; -1: none
        uint8_t type = 0, flags = 0;
        size_t bytes = kElementBytes;
        std::vector<int> kids;
    };
    std::vector<Node> nodes_;
    std::unordered_map<std::string, int> names_;  // interned ids
    std::unordered_map<int, int> by_id_;          // the first element with each id, as lunasvg's cache keeps
    size_t bytes_ = 0;

    int intern(const std::string& s) { return names_.emplace(s, int(names_.size())).first->second; }
    // IFRIT_SVG_BUDGET: a smaller budget in bytes, for tests.
    const size_t limit_ = std::getenv("IFRIT_SVG_BUDGET") ? std::strtoull(std::getenv("IFRIT_SVG_BUDGET"), nullptr, 10) : kMaxBytes;
    bool charge(size_t n) { return (bytes_ += n) <= limit_; }
    int add(Node n, int parent)
    {
        if (!charge(n.bytes))
            return -1;
        n.parent = parent, n.kids.clear();
        nodes_.push_back(std::move(n));
        const int i = int(nodes_.size()) - 1;
        if (parent >= 0)
            nodes_[parent].kids.push_back(i);
        return i;
    }
    // A copy of `from` under `parent` (SVGElement::clone), deep: its children
    // as they are now, copies included; -1 past the limits.
    int copy(int from, int parent, bool children)
    {
        const int to = add(nodes_[from], parent);
        if (to < 0)
            return -1;
        for (size_t k = 0; children && k < nodes_[from].kids.size(); ++k)
            if (copy(nodes_[from].kids[k], to, true) < 0)
                return -1;
        return to;
    }
    bool build(int n, int depth)
    {
        if (depth > kMaxDepth)
            return false;
        if (nodes_[n].flags & kUse)  // SVGUseElement::build and cloneTargetElement
        {
            const auto t = by_id_.find(nodes_[n].href);
            bool copy_it = nodes_[n].href >= 0 && t != by_id_.end() && t->second != n && (nodes_[t->second].flags & kClonable);
            for (int p = nodes_[n].parent; copy_it && p >= 0; p = nodes_[p].parent)
                copy_it = nodes_[p].id != nodes_[t->second].id;
            if (copy_it && copy(t->second, n, !(nodes_[t->second].flags & kUse)) < 0)
                return false;
        }
        for (size_t k = 0; k < nodes_[n].kids.size(); ++k)
            if (!build(nodes_[n].kids[k], depth + 1))
                return false;
        return true;
    }
};

constexpr bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
constexpr bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_num(char c) { return c >= '0' && c <= '9'; }
void skip_ws(std::string_view& in)
{
    while (!in.empty() && is_ws(in.front()))
        in.remove_prefix(1);
}
bool skip(std::string_view& in, std::string_view s)
{
    if (!in.starts_with(s))
        return false;
    in.remove_prefix(s.size());
    return true;
}
bool identifier(std::string_view& in, std::string& out)
{
    const auto start = [](char c) { return is_alpha(c) || c == '_' || c == ':'; };
    if (in.empty() || !start(in.front()))
        return false;
    size_t n = 1;
    while (n < in.size() && (start(in[n]) || is_num(in[n]) || in[n] == '-' || in[n] == '.'))
        ++n;
    out.assign(in.substr(0, n));
    in.remove_prefix(n);
    return true;
}

// An attribute value's character and entity references (decodeText): what's
// decoded until one it doesn't know, which is what lunasvg keeps.
std::string decode_text(std::string_view in)
{
    std::string out;
    while (!in.empty())
    {
        const char ch = in.front();
        in.remove_prefix(1);
        if (ch != '&')
        {
            out.push_back(ch);
            continue;
        }
        if (skip(in, "#"))
        {
            const int base = skip(in, "x") ? 16 : 10;
            const auto digit = [base](char c)
            {
                const int v = is_num(c) ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
                return v < base ? v : -1;
            };
            skip(in, "+");
            if (in.empty() || digit(in.front()) < 0)
                return out;
            uint64_t cp = 0;
            for (; !in.empty() && digit(in.front()) >= 0; in.remove_prefix(1))
                if ((cp = cp * base + digit(in.front())) > UINT32_MAX)
                    return out;
            char c[5] = {};
            if (cp < 0x80)
                c[0] = char(cp);
            else if (cp < 0x800)
                c[0] = char(0xC0 | (cp >> 6)), c[1] = char(0x80 | (cp & 0x3F));
            else if (cp < 0x10000)
                c[0] = char(0xE0 | (cp >> 12)), c[1] = char(0x80 | ((cp >> 6) & 0x3F)), c[2] = char(0x80 | (cp & 0x3F));
            else if (cp < 0x200000)
                c[0] = char(0xF0 | (cp >> 18)), c[1] = char(0x80 | ((cp >> 12) & 0x3F)), c[2] = char(0x80 | ((cp >> 6) & 0x3F)),
                c[3] = char(0x80 | (cp & 0x3F));
            out.append(c);
        }
        else if (skip(in, "amp"))
            out.push_back('&');
        else if (skip(in, "lt"))
            out.push_back('<');
        else if (skip(in, "gt"))
            out.push_back('>');
        else if (skip(in, "quot"))
            out.push_back('"');
        else if (skip(in, "apos"))
            out.push_back('\'');
        else
            return out;
        if (!skip(in, ";"))
            return out;
    }
    return out;
}

// lunasvg's elements (elementid), as 1...; 0 for one it doesn't know.
int element_type(const std::string& name)
{
    static constexpr std::string_view known[] = {"a", "circle", "clipPath", "defs", "ellipse", "g", "image", "line",
                                                 "linearGradient", "marker", "mask", "path", "pattern", "polygon",
                                                 "polyline", "radialGradient", "rect", "stop", "style", "svg", "symbol",
                                                 "text", "tspan", "use"};
    const auto it = std::find(std::begin(known), std::end(known), name);
    return it == std::end(known) ? 0 : int(it - std::begin(known)) + 1;
}

bool Model::parse(std::string_view in)
{
    static constexpr std::string_view clonable[] = {"a", "circle", "ellipse", "g", "image", "line", "path", "polygon",
                                                    "polyline", "rect", "svg", "symbol", "text", "tspan", "use"};
    std::string name;
    int current = -1, ignoring = 0, depth = 0;
    // Text kept (a text node: plain text, CDATA or a comment's) is part of
    // its element, and every copy of it holds its own.
    const auto text = [&](size_t n)
    {
        if (n == 0 || current < 0 || ignoring > 0 || !(nodes_[current].flags & kText))
            return true;
        nodes_[current].bytes += kTextBytes + n;
        return charge(kTextBytes + n);
    };
    while (!in.empty())
    {
        if (current >= 0)
        {
            const size_t n = std::min(in.find('<'), in.size());
            if (!text(n))
                return false;
            in.remove_prefix(n);
        }
        else if (skip_ws(in), in.empty())
            break;
        if (!skip(in, "<"))
            return false;
        if (skip(in, "?"))
        {
            if (!identifier(in, name) || in.find("?>") == in.npos)
                return false;
            in.remove_prefix(in.find("?>") + 2);
            continue;
        }
        if (skip(in, "!"))
        {
            const char* close = skip(in, "--") ? "-->" : skip(in, "[CDATA[") ? "]]>" : nullptr;
            if (close)
            {
                const size_t n = in.find(close);
                if (n == in.npos || !text(n))
                    return false;
                in.remove_prefix(n + 3);
                continue;
            }
            if (!skip(in, "DOCTYPE"))
                return false;
            while (!in.empty() && in.front() != '>')
            {
                if (in.front() == '[')
                {
                    in.remove_prefix(1);
                    for (int level = 1; !in.empty() && level > 0; in.remove_prefix(1))
                        level += in.front() == '[' ? 1 : in.front() == ']' ? -1 : 0;
                }
                else
                    in.remove_prefix(1);
            }
            if (!skip(in, ">"))
                return false;
            continue;
        }
        if (skip(in, "/"))
        {
            if ((current < 0 && ignoring == 0) || !identifier(in, name))
                return false;
            if (ignoring > 0)
                --ignoring;
            else if (element_type(name) != nodes_[current].type)
                return false;
            else
                current = nodes_[current].parent, --depth;
            skip_ws(in);
            if (!skip(in, ">"))
                return false;
            continue;
        }
        if (!identifier(in, name))
            return false;
        int element = -1;
        if (ignoring > 0)
            ++ignoring;
        else if (const int type = element_type(name); type == 0)
            ignoring = 1;
        else
        {
            if (nodes_.empty() ? name != "svg" : current < 0)
                return false;
            Node n;
            n.type = uint8_t(type);
            n.flags = uint8_t((name == "use" ? kUse : 0) | (name == "text" || name == "tspan" || name == "style" ? kText : 0) |
                              (std::find(std::begin(clonable), std::end(clonable), name) != std::end(clonable) ? kClonable : 0));
            if ((element = add(std::move(n), current)) < 0 || depth + 1 > kMaxDepth)
                return false;
        }
        skip_ws(in);
        while (identifier(in, name))
        {
            skip_ws(in);
            if (!skip(in, "="))
                return false;
            skip_ws(in);
            if (in.empty() || (in.front() != '"' && in.front() != '\''))
                return false;
            const char quote = in.front();
            in.remove_prefix(1);
            const size_t n = in.find(quote);
            if (n == in.npos)
                return false;
            if (element >= 0)
            {
                if (!charge(n))
                    return false;
                nodes_[element].bytes += n;
                if (name == "id")
                    by_id_.emplace(nodes_[element].id = intern(decode_text(in.substr(0, n))), element);
                else if (name == "href" || name == "xlink:href")
                {
                    const std::string href = decode_text(in.substr(0, n));
                    nodes_[element].href = href.starts_with('#') ? intern(href.substr(1)) : -1;
                }
            }
            in.remove_prefix(n + 1);
            skip_ws(in);
        }
        if (skip(in, ">"))
        {
            if (element >= 0)
                current = element, ++depth;
            continue;
        }
        if (!skip(in, "/") || !skip(in, ">"))
            return false;
        if (ignoring > 0)
            --ignoring;
    }
    return !nodes_.empty() && ignoring == 0 && in.empty();
}

// A gzip stream, inflated; empty if it isn't one (or inflates past 256 MB).
std::string gunzip(const uint8_t* d, size_t n)
{
    z_stream z{};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
        return {};
    z.next_in = const_cast<Bytef*>(d);
    z.avail_in = uInt(std::min<size_t>(n, UINT32_MAX));
    std::string out;
    char buf[1 << 16];
    int r;
    do
    {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        out.append(buf, sizeof buf - z.avail_out);
    } while (r == Z_OK && out.size() < (256u << 20));
    inflateEnd(&z);
    return r == Z_STREAM_END ? out : std::string{};
}

std::string size_text(double w, double h)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g × %g", w, h);
    return buf;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size))
    {
        error = "cannot read file";
        return false;
    }
    const bool gz = size >= 2 && d[0] == 0x1F && d[1] == 0x8B;
    try  // bad_alloc and the like refuse the file
    {
        std::string text = gz ? gunzip(d, size) : std::string(reinterpret_cast<const char*>(d), size);
        if (!svg_root(text))
        {
            error = gz ? "not a gzipped SVG" : "not an SVG file";
            return false;
        }
        if (text.starts_with("\xEF\xBB\xBF"))  // a byte-order mark, which lunasvg doesn't skip
            text.erase(0, 3);
        Model model;
        if (!model.parse(text))
        {
            error = "the SVG is malformed, or too large or too deeply nested to draw";
            return false;
        }
        if (!model.build())
        {
            error = "the SVG nests too deeply or repeats too much to draw";
            return false;
        }
        const auto doc = lunasvg::Document::loadFromData(text);
        const double w = doc ? doc->width() : 0, h = doc ? doc->height() : 0;
        if (!doc || !(w > 0 && h > 0 && std::isfinite(w) && std::isfinite(h)))
        {
            error = doc ? "the SVG has no size" : "lunasvg could not read the SVG";
            return false;
        }
        const double longer = std::max(w, h);
        const double scale = std::min(std::max(1.0, kMinSide / longer), kMaxSide / longer);
        const int rw = std::max(1, int(std::lround(w * scale))), rh = std::max(1, int(std::lround(h * scale)));
        lunasvg::Bitmap bitmap = doc->renderToBitmap(rw, rh);
        if (bitmap.isNull())
        {
            error = "lunasvg could not draw the SVG";
            return false;
        }
        out.allocate(rw, rh);
        out.from_bgra8(bitmap.data(), bitmap.stride(), true);  // premultiplied ARGB32, as Image keeps it
        out.has_alpha = !out.opaque();
        out.bits_per_sample = 8;
        out.format = gz ? "SVGZ" : "SVG";
        out.pixel_format.model = PixelFormat::Model::Vector;
        out.add(N_("SVG"), N_("Size"), size_text(w, h));
        const std::string view_box = doc->documentElement().getAttribute("viewBox");
        if (!view_box.empty())
            out.add(N_("SVG"), N_("View box"), view_box);
        out.add(N_("SVG"), N_("Rendered at"), size_text(rw, rh));
    }
    catch (const std::length_error&)
    {
        throw;  // Image::allocate: decode_image reports it
    }
    catch (const std::exception& e)
    {
        error = std::string("cannot draw the SVG: ") + e.what();
        return false;
    }
    return true;
}

}  // namespace

const Decoder& svg_decoder()
{
    static const Decoder d{"SVG", {"svg", "svgz"}, sniff, decode};
    return d;
}
