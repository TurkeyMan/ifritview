#pragma once
// XML as XMP and other small documents use it: elements, attributes, text,
// CDATA, the predefined entities and character references, namespaces;
// comments, processing instructions (<?xpacket?>) and a DOCTYPE skipped.
// Every element and attribute keeps its span of bytes in the source, so a
// document can be edited where it stands and the rest left byte for byte.
// Malformed input is rejected, not guessed at.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xml
{

constexpr std::string_view kXmlns = "http://www.w3.org/2000/xmlns/";                // xmlns:p="..." declares p
constexpr std::string_view kXmlNamespace = "http://www.w3.org/XML/1998/namespace";  // xml:lang

// Byte offsets into the source, end exclusive.
struct Span
{
    size_t begin = 0, end = 0;
};

struct Attribute
{
    // Its namespace (empty for none, as unprefixed attributes have) and
    // local name; a declaration has kXmlns and the prefix it declares ("" the
    // default namespace), its value the URI.
    std::string uri, name, qname;
    std::string value;  // entities resolved, whitespace normalised
    Span span;          // the name to past the closing quote
    Span value_span;    // between the quotes, as written
};

struct Element
{
    static constexpr size_t npos = size_t(-1);
    std::string uri, name, qname;  // namespace URI (empty for none), local name, as written
    std::vector<Attribute> attributes;
    std::vector<size_t> children;  // into Document::elements
    size_t parent = npos;
    std::string text;  // its own character data (not its children's), resolved
    Span span;         // '<' of its start tag to past its end tag
    Span tag;          // the start tag
    Span content;      // between the tags; at the start tag's end when self-closed
    bool self_closed = false;

    const Attribute* attribute(std::string_view uri, std::string_view name) const;
};

struct Document
{
    std::vector<Element> elements;  // in document order: the root first

    // The prefix bound to `uri` where `element` is ("" the default
    // namespace), if one is.
    std::optional<std::string> prefix(size_t element, std::string_view uri) const;
};

// XML's whitespace.
inline bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// False, and why, for input that isn't well-formed.
bool parse(std::string_view text, Document& doc, std::string& error);

// Text as an attribute's value or an element's content: & < > " ' escaped.
std::string escape(std::string_view text);

}  // namespace xml
