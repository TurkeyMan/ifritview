// XMP: the packet embedded in the file, plus a Lightroom/darktable style
// sidecar beside it (IMG.xmp or IMG.ext.xmp), read as RDF in any of its
// equivalent forms (xml.h). Everything is listed under its schema prefix;
// crs:* are Camera Raw's edit parameters. Saving edits the sidecar where it
// stands: the crs settings written change, every other byte stays.
#include "../i18n.h"
#include "../platform.h"
#include "../xml.h"
#include "decoder.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string_view>

namespace
{

constexpr int kMaxItems = 600;
constexpr size_t npos = xml::Element::npos;

constexpr std::string_view kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr std::string_view kCrs = "http://ns.adobe.com/camera-raw-settings/1.0/";
constexpr std::string_view kTiff = "http://ns.adobe.com/tiff/1.0/";

bool is_rdf(const xml::Element& e, std::string_view name)
{
    return e.uri == kRdf && e.name == name;
}

// An attribute that's a property (or a struct's field), not RDF's syntax,
// a declaration or xml:lang.
bool is_property(const xml::Attribute& a)
{
    return !a.uri.empty() && a.uri != kRdf && a.uri != xml::kXmlns && a.uri != xml::kXmlNamespace;
}

// An XMP packet: the properties are the attributes and the child elements
// of each rdf:Description in rdf:RDF.
struct Packet
{
    xml::Document doc;
    size_t rdf = npos;  // rdf:RDF
    std::vector<size_t> descriptions;
    struct Property
    {
        size_t description;
        size_t attribute = npos;  // crs:X="..." on it, or
        size_t element = npos;    // <crs:X>...</crs:X> in it
    };
    std::vector<Property> properties;

    bool parse(std::string_view text, std::string& error);
    const xml::Element& operator[](size_t element) const { return doc.elements[element]; }
    const xml::Attribute* attribute(const Property& p) const { return p.element == npos ? &doc.elements[p.description].attributes[p.attribute] : nullptr; }
    const std::string& uri(const Property& p) const { return p.element == npos ? attribute(p)->uri : doc.elements[p.element].uri; }
    const Property* find(std::string_view uri, std::string_view name) const;
    // A simple property's value; none for a struct or an array.
    std::optional<std::string> value(size_t element) const;
    std::optional<std::string> value(const Property& p) const { return p.element == npos ? attribute(p)->value : value(p.element); }
    std::optional<std::string> value(std::string_view uri, std::string_view name) const
    {
        const Property* p = find(uri, name);
        return p ? value(*p) : std::nullopt;
    }
    // An array's rdf:Seq, rdf:Bag or rdf:Alt, and its items in order, an
    // Alt's x-default first.
    size_t container(size_t element) const;
    std::vector<size_t> items(size_t container) const;
    std::vector<std::string> values(const Property* p) const;
    // The rdf:Description crs settings go in: the one they're in, or one
    // that has the namespace.
    size_t crs_description() const;
};

bool Packet::parse(std::string_view text, std::string& error)
{
    descriptions.clear();
    properties.clear();
    if (!xml::parse(text, doc, error))
        return false;
    const auto found = std::find_if(doc.elements.begin(), doc.elements.end(), [](const xml::Element& e) { return is_rdf(e, "RDF"); });
    if (found == doc.elements.end())
    {
        error = "no rdf:RDF";
        return false;
    }
    rdf = size_t(found - doc.elements.begin());
    for (const size_t d : found->children)
        if (is_rdf(doc.elements[d], "Description"))
        {
            descriptions.push_back(d);
            const xml::Element& e = doc.elements[d];
            for (size_t a = 0; a < e.attributes.size(); ++a)
                if (is_property(e.attributes[a]))
                    properties.push_back({d, a, npos});
            for (const size_t c : e.children)
                properties.push_back({d, npos, c});
        }
    return true;
}

const Packet::Property* Packet::find(std::string_view uri, std::string_view name) const
{
    for (const Property& p : properties)
        if (p.element == npos ? attribute(p)->uri == uri && attribute(p)->name == name
                              : doc.elements[p.element].uri == uri && doc.elements[p.element].name == name)
            return &p;
    return nullptr;
}

std::optional<std::string> Packet::value(size_t element) const
{
    const xml::Element& e = doc.elements[element];
    if (const xml::Attribute* resource = e.attribute(kRdf, "resource"))
        return resource->value;
    for (const size_t c : e.children)
        if (is_rdf(doc.elements[c], "value"))  // the value, the rest its qualifiers
            return value(c);
    const xml::Attribute* type = e.attribute(kRdf, "parseType");
    if (!e.children.empty() || (type && type->value == "Resource") || std::any_of(e.attributes.begin(), e.attributes.end(), is_property))
        return std::nullopt;
    return e.text;
}

size_t Packet::container(size_t element) const
{
    const xml::Element& e = doc.elements[element];
    if (e.children.size() != 1)
        return npos;
    const xml::Element& c = doc.elements[e.children[0]];
    return is_rdf(c, "Seq") || is_rdf(c, "Bag") || is_rdf(c, "Alt") ? e.children[0] : npos;
}

std::vector<size_t> Packet::items(size_t container) const
{
    std::vector<size_t> li;
    for (const size_t c : doc.elements[container].children)
        if (is_rdf(doc.elements[c], "li"))
            li.push_back(c);
    if (is_rdf(doc.elements[container], "Alt"))
        std::stable_partition(li.begin(), li.end(),
                              [&](size_t i)
                              {
                                  const xml::Attribute* lang = doc.elements[i].attribute(xml::kXmlNamespace, "lang");
                                  return lang && lang->value == "x-default";
                              });
    return li;
}

std::vector<std::string> Packet::values(const Property* p) const
{
    std::vector<std::string> out;
    const size_t c = p && p->element != npos ? container(p->element) : npos;
    if (c != npos)
        for (const size_t i : items(c))
            out.push_back(value(i).value_or(""));
    return out;
}

size_t Packet::crs_description() const
{
    for (const Property& p : properties)
        if (uri(p) == kCrs && !doc.prefix(p.description, kCrs).value_or("").empty())
            return p.description;
    for (const size_t d : descriptions)
        if (!doc.prefix(d, kCrs).value_or("").empty())
            return d;
    return npos;
}

using Leaves = std::vector<std::pair<std::string, std::string>>;
void fields(const Packet& p, size_t element, const std::string& path, Leaves& out);

// Each leaf of a property, by its path: "dc:subject[2]",
// "xmpMM:History[1]/stEvt:action".
void leaves(const Packet& p, size_t element, const std::string& path, Leaves& out)
{
    if (std::optional<std::string> v = p.value(element))
        out.emplace_back(path, std::move(*v));
    else if (const size_t c = p.container(element); c != npos)
    {
        int n = 0;
        for (const size_t i : p.items(c))
            leaves(p, i, path + "[" + std::to_string(++n) + "]", out);
    }
    else
        fields(p, element, path + "/", out);
}

// A struct's fields (or a Description's properties): its attributes, its
// elements, those of an rdf:Description in it.
void fields(const Packet& p, size_t element, const std::string& path, Leaves& out)
{
    for (const xml::Attribute& a : p[element].attributes)
        if (is_property(a))
            out.emplace_back(path + a.qname, a.value);
    for (const size_t c : p[element].children)
        if (is_rdf(p[c], "Description"))
            fields(p, c, path, out);
        else
            leaves(p, c, path + p[c].qname, out);
}

// Every leaf, a schema's together in the order they first come, as
// "prefix:path".
void collect(const Packet& p, const std::string& group, Image& out)
{
    std::vector<std::pair<std::string_view, Leaves>> schemas;
    for (const Packet::Property& property : p.properties)
    {
        const std::string_view uri = p.uri(property);
        auto schema = std::find_if(schemas.begin(), schemas.end(), [&](const auto& s) { return s.first == uri; });
        if (schema == schemas.end())
            schema = schemas.insert(schemas.end(), {uri, {}});
        if (const xml::Attribute* a = p.attribute(property))
            schema->second.emplace_back(a->qname, a->value);
        else
            leaves(p, property.element, p[property.element].qname, schema->second);
    }
    int count = 0;
    for (auto& [uri, items] : schemas)
        for (auto& [key, val] : items)
        {
            if (count >= kMaxItems)
                return;
            if (val.empty())
                continue;
            // Trim noise: "dc:subject[1]" reads better as "dc:subject".
            if (key.size() > 3 && key.find("[1]") == key.size() - 3)
                key.erase(key.size() - 3);
            if (val.size() > 500)
                val = val.substr(0, 500) + "…";
            out.add(group, key, val);
            ++count;
        }
}

bool get_double(const Packet& p, std::string_view ns, std::string_view name, double& value)
{
    const std::optional<std::string> s = p.value(ns, name);
    if (!s || s->empty())
        return false;
    value = g_ascii_strtod(s->c_str(), nullptr);
    return true;
}

std::string get_string(const Packet& p, std::string_view ns, std::string_view name)
{
    return p.value(ns, name).value_or("");
}

// Later sources (a sidecar) override earlier ones (the embedded packet).
void collect_develop(const Packet& p, Image& out)
{
    XmpDevelop& d = out.xmp;
    double v = 0;
    if (get_double(p, kCrs, "Exposure2012", v) || get_double(p, kCrs, "Exposure", v))
    {
        d.exposure = v;
        d.present = true;
    }
    const std::string wb = get_string(p, kCrs, "WhiteBalance");
    if (wb != "As Shot" && get_double(p, kCrs, "Temperature", v) && v > 0)
    {
        d.temperature = v;
        d.tint = get_double(p, kCrs, "Tint", v) ? v : 0;
        d.present = true;
    }
    if (get_double(p, kTiff, "Orientation", v) && v >= 1 && v <= 8)
    {
        d.orientation = int(v);
        d.present = true;
    }
    // The process its settings are for, then them.
    Adjustments& a = d.adjust;
    const std::string process = get_string(p, kCrs, "ProcessVersion");
    if (!process.empty())
    {
        d.process_version = process;
        a.legacy = g_ascii_strtod(process.c_str(), nullptr) < 6;
    }
    else if (!p.find(kCrs, "Contrast2012") && (p.find(kCrs, "Brightness") || p.find(kCrs, "FillLight")))
        a.legacy = true;
    for (const CrsField& f : crs_fields(a))
        if (get_double(p, kCrs, f.name, v))
        {
            *f.value = v;
            d.given.insert(f.name);
            d.present = true;
        }
    for (const CrsCurve& c : crs_curves(a))
    {
        Adjustments::Curve points;
        for (const std::string& item : p.values(p.find(kCrs, c.name)))
        {
            int x = 0, y = 0;
            if (std::sscanf(item.c_str(), "%d , %d", &x, &y) == 2)
                points.emplace_back(x, y);
        }
        if (!points.empty())
        {
            *c.curve = std::move(points);
            d.present = true;
        }
    }
    if (std::string name = get_string(p, kCrs, crs_curve_name(a)); !name.empty())
        a.curve_name = std::move(name);
    // The rest of Camera Raw's simple settings, not applied (yet), kept for
    // writing them back.
    std::vector<std::string> owned = {"ProcessVersion", "Exposure",      "Exposure2012",      "WhiteBalance", "Temperature",
                                      "Tint",           "ToneCurveName", "ToneCurveName2012", "HasSettings"};
    for (const CrsField& f : crs_fields(a))
        owned.push_back(f.name);
    for (const Packet::Property& property : p.properties)
        if (p.uri(property) == kCrs)
        {
            const std::string& name = property.element == npos ? p.attribute(property)->name : p[property.element].name;
            if (std::find(owned.begin(), owned.end(), name) == owned.end())
                if (std::optional<std::string> value = p.value(property))
                    a.kept[name] = std::move(*value);
        }
    const std::string has_crop = get_string(p, kCrs, "HasCrop");
    if (has_crop == "True" || has_crop == "true")
    {
        double l = 0, t = 0, r = 1, b = 1;
        get_double(p, kCrs, "CropLeft", l);
        get_double(p, kCrs, "CropTop", t);
        get_double(p, kCrs, "CropRight", r);
        get_double(p, kCrs, "CropBottom", b);
        if (r > l && b > t)
        {
            d.has_crop = true;
            d.crop_left = l;
            d.crop_top = t;
            d.crop_right = r;
            d.crop_bottom = b;
            d.present = true;
        }
    }
}

bool read_sidecar(const std::string& sidecar, Image& out)
{
    gchar* text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(sidecar.c_str(), &text, &len, nullptr))
        return false;
    Packet p;
    std::string error;
    const bool ok = p.parse({text, len}, error);
    if (ok)
    {
        gchar* base = g_path_get_basename(sidecar.c_str());
        out.add(N_("XMP sidecar"), N_("File"), base);
        g_free(base);
        collect(p, "XMP sidecar", out);
        collect_develop(p, out);
    }
    g_free(text);
    return ok;
}

}  // namespace

// The embedded XMP packet sits verbatim in the file (JPEG APP1, PNG iTXt,
// TIFF/DNG tag, WebP chunk...); find it in the bytes and parse just that.
void read_xmp(const std::string& path, const uint8_t* data, size_t size, Image& out)
{
    if (data && size)
    {
        const std::string_view all(reinterpret_cast<const char*>(data), size);
        const size_t start = all.find("<x:xmpmeta");
        const size_t end = start == std::string_view::npos ? start : all.find("</x:xmpmeta>", start);
        Packet p;
        std::string error;
        if (end != std::string_view::npos && p.parse(all.substr(start, end + 12 - start), error))
        {
            collect(p, "XMP", out);
            collect_develop(p, out);
        }
    }
    // Sidecars: IMG_0001.CR2.xmp (darktable), then IMG_0001.xmp (Lightroom's,
    // and what IfritView writes): the last one read has the last word.
    read_sidecar(path + ".xmp", out);
    if (const std::string lightroom = xmp_sidecar_path(path); lightroom != path + ".xmp")
        read_sidecar(lightroom, out);
}

std::string xmp_sidecar_path(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    const auto slash = path.find_last_of("/\\");  // either slash: a dot in a folder's name isn't one
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return path.substr(0, dot) + ".xmp";
    return path + ".xmp";
}

namespace
{

// A setting as Lightroom writes it, whatever the locale: "+15", "-0.35",
// zero unsigned.
std::string crs_number(double v, const char* format = "%+.0f")
{
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(text, sizeof text, format, v);
    if (text[0] == '+' || text[0] == '-')
        if (g_ascii_strtod(text + 1, nullptr) == 0)
            return text + 1;
    return text;
}

// A sidecar of IfritView's own, for a photo without one: Lightroom's
// layout, the settings to come.
const char kFreshSidecar[] = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"IfritView " IFRIT_VERSION
                             "\">\n"
                             " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                             "  <rdf:Description rdf:about=\"\"\n"
                             "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\">\n"
                             "  </rdf:Description>\n"
                             " </rdf:RDF>\n"
                             "</x:xmpmeta>\n";

std::string qualified(const std::string& prefix, std::string_view name)
{
    return prefix.empty() ? std::string(name) : prefix + ":" + std::string(name);
}

// The whitespace that ends at `at`.
std::string_view space_before(std::string_view text, size_t at)
{
    size_t start = at;
    while (start > 0 && xml::space(text[start - 1]))
        --start;
    return text.substr(start, at - start);
}

// The whitespace a new child of `e` comes after: its last child's, or one
// space more than before its end (its start, when it's self-closed).
std::string child_space(std::string_view text, const Packet& p, const xml::Element& e)
{
    if (!e.children.empty())
        return std::string(space_before(text, p[e.children.back()].span.begin));
    return std::string(space_before(text, e.self_closed ? e.span.begin : e.content.end)) + " ";
}

// The text from `begin` to `end` becomes `text`.
struct Edit
{
    size_t begin, end;
    std::string text;
};

// `children` (each after its whitespace) added at the end of `e`'s content.
Edit append(std::string_view text, const xml::Element& e, const std::string& children)
{
    if (!e.self_closed)
    {
        const size_t at = e.content.end - space_before(text, e.content.end).size();
        return {at, at, children};
    }
    return {e.tag.end - 2, e.tag.end, ">" + children + std::string(space_before(text, e.span.begin)) + "</" + e.qname + ">"};
}

std::string apply(std::string_view text, std::vector<Edit> edits)
{
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin < b.begin; });
    std::string out;
    size_t at = 0;
    for (const Edit& e : edits)
    {
        out.append(text.substr(at, e.begin - at));
        out += e.text;
        at = e.end;
    }
    out.append(text.substr(at));
    return out;
}

// Camera Raw's settings set in a sidecar's text, where they stand: a value
// that changes replaced, one that's missing added to the crs rdf:Description
// (as an attribute where its settings are attributes, an array as an
// element), one removed. Nothing else is touched.
class Merge
{
public:
    Merge(std::string_view text, const Packet& p, size_t description)
        : text_(text),
          p_(p),
          description_(p[description]),
          crs_(*p.doc.prefix(description, kCrs)),
          rdf_(p.doc.prefix(description, kRdf).value_or("")),
          space_(child_space(text, p, description_))
    {
        // Laid out as its last property attribute, or one space in from it.
        const auto last = std::find_if(description_.attributes.rbegin(), description_.attributes.rend(), is_property);
        attribute_space_ = last != description_.attributes.rend() ? std::string(space_before(text, last->span.begin))
                                                                  : std::string(space_before(text, description_.span.begin)) + " ";
        // As attributes, unless its settings are elements.
        attributes_ =
            std::none_of(p.properties.begin(), p.properties.end(), [&](const Packet::Property& q) { return q.element != npos && p.uri(q) == kCrs; }) ||
            std::any_of(p.properties.begin(), p.properties.end(), [&](const Packet::Property& q) { return q.element == npos && p.uri(q) == kCrs; });
    }

    bool has(std::string_view name) const { return p_.find(kCrs, name) || std::find(added_.begin(), added_.end(), name) != added_.end(); }

    void set(std::string_view name, const std::string& value)
    {
        if (const Packet::Property* q = p_.find(kCrs, name))
        {
            if (p_.value(*q) == value)
                return;
            if (const xml::Attribute* a = p_.attribute(*q))
                edits_.push_back({a->value_span.begin, a->value_span.end, xml::escape(value)});
            else if (const xml::Element& e = p_[q->element]; e.children.empty() && !e.self_closed && !e.attribute(kRdf, "resource"))
                edits_.push_back({e.content.begin, e.content.end, xml::escape(value)});
            else
                edits_.push_back({e.span.begin, e.span.end, element(e.qname, value)});
        }
        else if (attributes_)
            add(name, attribute_space_ + qualified(crs_, name) + "=\"" + xml::escape(value) + "\"", new_attributes_);
        else
            add(name, space_ + element(qualified(crs_, name), value), new_elements_);
    }

    // An array (rdf:Seq) of `items`.
    void set(std::string_view name, const std::vector<std::string>& items)
    {
        const Packet::Property* q = p_.find(kCrs, name);
        if (q && p_.values(q) == items)
            return;
        if (const size_t c = q && q->element != npos ? p_.container(q->element) : npos; c != npos && !p_.items(c).empty())
        {
            // The items replaced, laid out as the first was.
            const std::vector<size_t> li = p_.items(c);
            const std::string space(space_before(text_, p_[li.front()].span.begin));
            std::string seq;
            for (const std::string& item : items)
                seq += space + element(p_[li.front()].qname, item);
            const xml::Element& container = p_[c];
            seq += space_before(text_, container.content.end);
            edits_.push_back({container.content.begin, container.content.end, std::move(seq)});
            return;
        }
        remove(name);
        std::string seq = space_ + "<" + qualified(crs_, name) + ">" + space_ + " <" + qualified(rdf_, "Seq") + ">";
        for (const std::string& item : items)
            seq += space_ + "  " + element(qualified(rdf_, "li"), item);
        seq += space_ + " </" + qualified(rdf_, "Seq") + ">" + space_ + "</" + qualified(crs_, name) + ">";
        add(name, seq, new_elements_);
    }

    void remove(std::string_view name)
    {
        if (const Packet::Property* q = p_.find(kCrs, name))
        {
            const xml::Span span = q->element == npos ? p_.attribute(*q)->span : p_[q->element].span;
            edits_.push_back({span.begin - space_before(text_, span.begin).size(), span.end, ""});
        }
    }

    std::string result() const
    {
        std::vector<Edit> edits = edits_;
        if (!new_attributes_.empty())
        {
            const size_t at = description_.tag.end - (description_.self_closed ? 2 : 1);
            edits.push_back({at, at, new_attributes_});
        }
        if (!new_elements_.empty())
            edits.push_back(append(text_, description_, new_elements_));
        return apply(text_, std::move(edits));
    }

private:
    static std::string element(const std::string& qname, const std::string& value) { return "<" + qname + ">" + xml::escape(value) + "</" + qname + ">"; }

    void add(std::string_view name, const std::string& markup, std::string& to)
    {
        added_.emplace_back(name);
        to += markup;
    }

    std::string_view text_;
    const Packet& p_;
    const xml::Element& description_;
    std::string crs_, rdf_;                // their prefixes there
    std::string space_, attribute_space_;  // before a new element, a new attribute
    bool attributes_;
    std::vector<Edit> edits_;
    std::vector<std::string> added_;             // the settings added, by name
    std::string new_attributes_, new_elements_;  // and their markup
};

// An rdf:Description declaring crs added to rdf:RDF, about what the others are.
std::string with_crs_description(std::string_view text, const Packet& p)
{
    const xml::Element& rdf = p[p.rdf];
    const std::string prefix = p.doc.prefix(p.rdf, kRdf).value_or("");
    const xml::Attribute* about = p.descriptions.empty() ? nullptr : p[p.descriptions.front()].attribute(kRdf, "about");
    const std::string space = child_space(text, p, rdf), description = qualified(prefix, "Description");
    return apply(text, {append(text, rdf,
                               space + "<" + description + " " + qualified(prefix, "about") + "=\"" + xml::escape(about ? about->value : "") + "\"" + space +
                                   "  xmlns:crs=\"" + std::string(kCrs) + "\">" + space + "</" + description + ">")});
}

}  // namespace

bool write_xmp_sidecar(const std::string& path, const XmpDevelop& edits, std::string& error)
{
    const std::string sidecar = xmp_sidecar_path(path);
    // What's there already, all of it kept but what's set below; one that
    // can't be read is left as it is.
    std::string text = kFreshSidecar;
    gchar* contents = nullptr;
    gsize len = 0;
    if (g_file_get_contents(sidecar.c_str(), &contents, &len, nullptr))
    {
        text.assign(contents, len);
        g_free(contents);
    }
    Packet p;
    std::string why;
    if (!p.parse(text, why))
    {
        error = rap::i18n::format(_("{0} isn't XMP that can be read: left as it is"), sidecar);
        return false;
    }
    if (p.crs_description() == npos)
    {
        text = with_crs_description(text, p);
        p.parse(text, why);
    }
    Merge xmp(text, p, p.crs_description());

    Adjustments a = edits.adjust;
    // Settings it read and doesn't apply yet go back where they're missing.
    for (const auto& [name, value] : a.kept)
        if (!xmp.has(name))
            xmp.set(name, value);
    std::string process = edits.process_version;
    if (process.empty() || (g_ascii_strtod(process.c_str(), nullptr) < 6) != a.legacy)
        process = a.legacy ? "5.7" : "11.0";
    xmp.set("ProcessVersion", process);
    xmp.set(a.legacy ? "Exposure" : "Exposure2012", crs_number(edits.exposure, "%+.2f"));
    if (edits.temperature > 0)
    {
        xmp.set("WhiteBalance", "Custom");
        xmp.set("Temperature", crs_number(edits.temperature, "%.0f"));
        xmp.set("Tint", crs_number(edits.tint));
    }
    else
    {
        // As shot: the camera's, which Lightroom reads from the raw.
        xmp.set("WhiteBalance", "As Shot");
        xmp.remove("Temperature");
        xmp.remove("Tint");
    }
    for (const CrsField& f : crs_fields(a))
        if (!f.process || f.process == (a.legacy ? 2010 : 2012))
            xmp.set(f.name, crs_number(*f.value, !f.sign ? "%.0f" : f.decimals ? "%+.1f" : "%+.0f"));
    for (const CrsCurve& c : crs_curves(a))
    {
        Adjustments::Curve points = *c.curve;
        if (points.size() < 2)
            points = {{0, 0}, {255, 255}};
        std::vector<std::string> items;
        for (const auto& [x, y] : points)
            items.push_back(std::to_string(x) + ", " + std::to_string(y));
        xmp.set(c.name, items);
    }
    const bool linear = a.curve.size() < 2 || a.curve == Adjustments::Curve{{0, 0}, {255, 255}};
    xmp.set(crs_curve_name(a), !a.curve_name.empty() ? a.curve_name : linear ? "Linear" : "Custom");
    xmp.set("HasSettings", "True");

    const std::string bytes = xmp.result();
    return platform::write_replacing(
        sidecar,
        [&](const std::string& temp, std::string& why)
        {
            GError* e = nullptr;
            if (g_file_set_contents(temp.c_str(), bytes.data(), gssize(bytes.size()), &e))
                return true;
            why = e ? e->message : "";
            g_clear_error(&e);
            return false;
        },
        error);
}
