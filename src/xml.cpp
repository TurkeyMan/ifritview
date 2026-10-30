#include "xml.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace xml
{

const Attribute* Element::attribute(std::string_view u, std::string_view n) const
{
    for (const Attribute& a : attributes)
        if (a.uri == u && a.name == n)
            return &a;
    return nullptr;
}

std::optional<std::string> Document::prefix(size_t element, std::string_view uri) const
{
    if (uri == kXmlNamespace)
        return "xml";
    std::vector<std::string_view> bound;  // nearer declarations hide farther ones of the prefix
    for (size_t e = element; e != Element::npos; e = elements[e].parent)
        for (const Attribute& a : elements[e].attributes)
            if (a.uri == kXmlns)
            {
                if (a.value == uri && std::find(bound.begin(), bound.end(), a.name) == bound.end())
                    return a.name;
                bound.push_back(a.name);
            }
    return std::nullopt;
}

std::string escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        switch (c)
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
        }
    return out;
}

namespace
{

void utf8(uint32_t c, std::string& out)
{
    if (c < 0x80)
    {
        out += char(c);
        return;
    }
    const int extra = c < 0x800 ? 1 : c < 0x10000 ? 2 : 3;  // continuation bytes
    out += char(uint8_t(0xff00 >> (extra + 1)) | c >> 6 * extra);
    for (int k = extra; k-- > 0;)
        out += char(0x80 | (c >> 6 * k & 0x3f));
}

struct Parser
{
    Parser(std::string_view text, Document& d, std::string& e) : s(text), doc(d), error(e) {}

    std::string_view s;
    Document& doc;
    std::string& error;
    size_t i = 0;
    std::vector<size_t> open;                                                                        // elements whose end tag is to come
    std::vector<std::pair<std::string, std::string>> scope = {{"xml", std::string(kXmlNamespace)}};  // prefix, URI
    std::vector<size_t> scope_size;                                                                  // `scope` before each open element's declarations

    bool fail(const char* what)
    {
        error = std::string(what) + " at byte " + std::to_string(i);
        return false;
    }

    bool at(std::string_view token) const { return s.substr(i, token.size()) == token; }

    // Past `token`, or false at the end.
    bool skip_past(std::string_view token)
    {
        const size_t at = s.find(token, i);
        if (at == std::string_view::npos)
            return false;
        i = at + token.size();
        return true;
    }

    std::string_view name()
    {
        const size_t start = i;
        while (i < s.size() && !space(s[i]) && !std::strchr("/>=<\"'&?!", s[i]))
            ++i;
        const std::string_view n = s.substr(start, i - start);
        if (!n.empty() && (n[0] == '-' || n[0] == '.' || (n[0] >= '0' && n[0] <= '9') || n.front() == ':' || n.back() == ':'))
            return {};
        return n;
    }

    // Character data with its references resolved and its line ends made
    // \n; in an attribute's value, any whitespace a space.
    bool decode(std::string_view raw, bool attribute, std::string& out)
    {
        for (size_t k = 0; k < raw.size(); ++k)
        {
            char c = raw[k];
            if (c == '\r')
            {
                if (k + 1 < raw.size() && raw[k + 1] == '\n')
                    ++k;
                c = '\n';
            }
            if (attribute && (c == '\n' || c == '\t'))
                c = ' ';
            if (c != '&')
            {
                if (c == '<')
                    return fail("'<' in an attribute's value");
                out += c;
                continue;
            }
            const size_t semi = raw.find(';', k);
            if (semi == std::string_view::npos)
                return fail("an unterminated reference");
            const std::string_view ref = raw.substr(k + 1, semi - k - 1);
            k = semi;
            if (ref == "lt")
                out += '<';
            else if (ref == "gt")
                out += '>';
            else if (ref == "amp")
                out += '&';
            else if (ref == "quot")
                out += '"';
            else if (ref == "apos")
                out += '\'';
            else if (ref.size() > 1 && ref[0] == '#')
            {
                const bool hex = ref[1] == 'x';
                const std::string_view digits = ref.substr(hex ? 2 : 1);
                uint32_t code = 0;
                for (const char d : digits)
                {
                    const int v = d >= '0' && d <= '9' ? d - '0' : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10 : hex && d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
                    if (v < 0 || code > 0x10ffff)
                        return fail("a bad character reference");
                    code = code * (hex ? 16 : 10) + uint32_t(v);
                }
                if (digits.empty() || code == 0 || code > 0x10ffff || (code >= 0xd800 && code < 0xe000))
                    return fail("a bad character reference");
                utf8(code, out);
            }
            else
                return fail("an unknown entity");
        }
        return true;
    }

    // `qname` as (URI, local name), by the declarations in scope; an
    // unprefixed attribute is in no namespace.
    bool resolve(std::string_view qname, bool attribute, std::string& uri, std::string& local)
    {
        const size_t colon = qname.find(':');
        const std::string_view prefix = colon == std::string_view::npos ? std::string_view() : qname.substr(0, colon);
        local = qname.substr(colon == std::string_view::npos ? 0 : colon + 1);
        uri.clear();
        if (prefix.empty() && attribute)
            return true;
        for (size_t k = scope.size(); k-- > 0;)
            if (scope[k].first == prefix)
            {
                uri = scope[k].second;
                return true;
            }
        return prefix.empty() || fail("an undeclared namespace prefix");
    }

    // Comments, processing instructions, a DOCTYPE: skipped.
    bool misc(bool& skipped)
    {
        skipped = true;
        if (at("<!--"))
            return skip_past("-->") || fail("an unterminated comment");
        if (at("<?"))
            return skip_past("?>") || fail("an unterminated processing instruction");
        if (at("<!DOCTYPE"))
        {
            int depth = 0;
            for (; i < s.size(); ++i)
                if (s[i] == '[')
                    ++depth;
                else if (s[i] == ']')
                    --depth;
                else if (s[i] == '>' && depth == 0)
                {
                    ++i;
                    return true;
                }
            return fail("an unterminated DOCTYPE");
        }
        skipped = false;
        return true;
    }

    bool start_tag()
    {
        Element e;
        e.span.begin = e.tag.begin = i++;
        const std::string_view qname = name();
        if (qname.empty())
            return fail("a bad element name");
        e.qname = qname;
        struct Raw
        {
            std::string_view qname;
            Attribute a;
        };
        std::vector<Raw> raw;
        for (;;)
        {
            const size_t before = i;
            while (i < s.size() && space(s[i]))
                ++i;
            if (i >= s.size())
                return fail("an unterminated start tag");
            if (s[i] == '>' || at("/>"))
                break;
            if (i == before)
                return fail("attributes without space between");
            Raw r;
            r.a.span.begin = i;
            r.qname = name();
            if (r.qname.empty())
                return fail("a bad attribute name");
            while (i < s.size() && space(s[i]))
                ++i;
            if (i >= s.size() || s[i] != '=')
                return fail("an attribute without a value");
            ++i;
            while (i < s.size() && space(s[i]))
                ++i;
            if (i >= s.size() || (s[i] != '"' && s[i] != '\''))
                return fail("an attribute's value unquoted");
            const size_t close = s.find(s[i], i + 1);
            if (close == std::string_view::npos)
                return fail("an unterminated attribute value");
            r.a.value_span = {i + 1, close};
            r.a.span.end = close + 1;
            if (!decode(s.substr(i + 1, close - i - 1), true, r.a.value))
                return false;
            i = close + 1;
            for (const Raw& other : raw)
                if (other.qname == r.qname)
                    return fail("an attribute given twice");
            raw.push_back(std::move(r));
        }
        e.self_closed = s[i] == '/';
        i += e.self_closed ? 2 : 1;
        e.tag.end = i;
        e.content = {i, i};

        // Its declarations first: they apply to its own name and attributes.
        scope_size.push_back(scope.size());
        for (Raw& r : raw)
            if (r.qname == "xmlns" || r.qname.starts_with("xmlns:"))
            {
                r.a.uri = kXmlns;
                r.a.name = r.qname.size() > 5 ? r.qname.substr(6) : std::string_view();
                scope.emplace_back(r.a.name, r.a.value);
            }
        if (!resolve(qname, false, e.uri, e.name))
            return false;
        for (Raw& r : raw)
        {
            r.a.qname = r.qname;
            if (r.a.uri.empty() && !resolve(r.qname, true, r.a.uri, r.a.name))
                return false;
            e.attributes.push_back(std::move(r.a));
        }
        const size_t index = doc.elements.size();
        if (!open.empty())
        {
            e.parent = open.back();
            doc.elements[e.parent].children.push_back(index);
        }
        doc.elements.push_back(std::move(e));
        if (doc.elements.back().self_closed)
        {
            doc.elements.back().span.end = i;
            scope.resize(scope_size.back());
            scope_size.pop_back();
        }
        else
            open.push_back(index);
        return true;
    }

    bool end_tag()
    {
        const size_t begin = i;
        i += 2;
        const std::string_view qname = name();
        while (i < s.size() && space(s[i]))
            ++i;
        if (i >= s.size() || s[i] != '>')
            return fail("a bad end tag");
        ++i;
        Element& e = doc.elements[open.back()];
        if (qname != e.qname)
            return fail("an end tag that doesn't match its start");
        e.content.end = begin;
        e.span.end = i;
        open.pop_back();
        scope.resize(scope_size.back());
        scope_size.pop_back();
        return true;
    }

    bool run()
    {
        if (at("\xef\xbb\xbf"))
            i = 3;
        bool root = false;
        while (i < s.size())
        {
            if (open.empty())
            {
                // Outside the root: whitespace and markup that's skipped.
                bool skipped = false;
                if (space(s[i]))
                    ++i;
                else if (!misc(skipped))
                    return false;
                else if (skipped)
                    continue;
                else if (s[i] == '<' && !root && !at("</") && !at("<!"))
                {
                    root = true;
                    if (!start_tag())
                        return false;
                }
                else
                    return fail(root ? "content after the root element" : "content before the root element");
                continue;
            }
            Element& e = doc.elements[open.back()];
            if (s[i] != '<')
            {
                const size_t end = std::min(s.find('<', i), s.size());
                std::string text;
                if (!decode(s.substr(i, end - i), false, text))
                    return false;
                e.text += text;
                i = end;
                continue;
            }
            bool skipped = false;
            if (at("<![CDATA["))
            {
                const size_t start = i + 9;
                if (!skip_past("]]>"))
                    return fail("an unterminated CDATA section");
                for (size_t k = start; k < i - 3; ++k)
                    if (s[k] != '\r')
                        e.text += s[k];
                    else if (k + 1 >= i - 3 || s[k + 1] != '\n')
                        e.text += '\n';
            }
            else if (at("</"))
            {
                if (!end_tag())
                    return false;
            }
            else if (!misc(skipped))
                return false;
            else if (!skipped && !start_tag())
                return false;
        }
        if (!open.empty())
            return fail("an element left open");
        return root || fail("no root element");
    }
};

}  // namespace

bool parse(std::string_view text, Document& doc, std::string& error)
{
    doc.elements.clear();
    Parser p(text, doc, error);
    return p.run();
}

}  // namespace xml
