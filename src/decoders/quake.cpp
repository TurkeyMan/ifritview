// Quake's 8-bit images, which carry no palette: the game's palette is a
// file of its own. Quake 2 WAL textures: a 32-byte name, width, height,
// four mipmap offsets, the next animation frame's name and the surface
// flags, then the mipmaps (the largest is shown). Quake 1 LMP pictures:
// the "qpic" layout (32-bit width and height, then the pixels), and the
// headerless lumps beside them: palette.lmp (shown as a 16 × 16 swatch),
// colormap.lmp (256 × 64 lighting ramps) and conchars (the 128 × 128 font).
// The palette is looked for near the file, walking up the folders: a WAL
// wants pics/colormap.pcx (its trailing VGA palette), an LMP gfx/palette.lmp
// or a palette.lmp beside it. Without one the indices are shown as grey.
// Index 255 is transparent in both games' pictures (Quake 2's renderer
// gives every 8-bit image that); conchars uses index 0 instead.
#include "decoder.h"
#include "../i18n.h"
#include "../bytes.h"

#include "../platform.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

// The whole of a small file, or empty (missing, unreadable, too big).
std::vector<uint8_t> read_small(const std::string& path, size_t max)
{
    std::vector<uint8_t> d;
    FILE* f = platform::fopen(path, "rb");
    if (!f)
        return d;
    d.resize(max + 1);
    d.resize(std::fread(d.data(), 1, d.size(), f));
    std::fclose(f);
    if (d.size() > max)
        d.clear();
    return d;
}

std::string folder(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string base_name(const std::string& path)
{
    std::string s = path.substr(path.find_last_of("/\\") + 1);  // npos + 1 == 0
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// Quake 1's palette: 256 RGB triples, nothing else.
bool palette_lmp(const std::vector<uint8_t>& d, uint32_t palette[256])
{
    if (d.size() != 768)
        return false;
    for (int i = 0; i < 256; ++i)
        palette[i] = 0xFF000000u | uint32_t(d[i * 3]) << 16 | uint32_t(d[i * 3 + 1]) << 8 | d[i * 3 + 2];
    return true;
}

// Loads the first of `names` (paths relative to a folder) that reads as a
// palette, trying the file's own folder, then each one above it; `beside`
// is tried in the file's folder only. Returns the path found, or empty.
std::string find_palette(const std::string& path, const std::vector<std::string>& beside, const std::vector<std::string>& names, bool pcx,
                         uint32_t palette[256])
{
    auto load = [&](const std::string& dir, const std::string& name)
    {
        const std::string candidate = (dir.empty() ? std::string() : dir + "/") + name;
        const std::vector<uint8_t> d = read_small(candidate, pcx ? 1u << 20 : 768);
        return !d.empty() && (pcx ? pcx_vga_palette(d.data(), d.size(), palette) : palette_lmp(d, palette)) ? candidate : std::string();
    };
    std::string dir = folder(path);
    for (const std::string& name : beside)
        if (std::string found = load(dir, name); !found.empty())
            return found;
    for (int depth = 0; depth < 8; ++depth)
    {
        for (const std::string& name : names)
            if (std::string found = load(dir, name); !found.empty())
                return found;
        const std::string up = folder(dir);
        if (up.empty() || up == dir)
            break;
        dir = up;
    }
    return std::string();
}

// Fills `out` from w × h indices through the palette (grey without one),
// with `clear` (or none, -1) transparent.
void put_indexed(const uint8_t* px, int w, int h, const uint32_t* palette, int clear, Image& out)
{
    out.allocate(w, h);
    bool any_clear = false;
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* s = px + size_t(y) * w;
        uint32_t* o = out.row(y);
        for (int x = 0; x < w; ++x)
        {
            if (s[x] == clear)
            {
                o[x] = 0;
                any_clear = true;
            }
            else
                o[x] = palette ? palette[s[x]] : 0xFF000000u | s[x] * 0x010101u;
        }
    }
    out.has_alpha = any_clear;
    out.bits_per_sample = 8;
    out.pixel_format = PixelFormat::palette(8, 256);
}

void note_palette(const std::string& group, const std::string& found, Image& out)
{
    out.add(group, "Palette", found.empty() ? "not found, shown in grey" : found);
}

bool decode_wal(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 100)
    {
        error = "not a WAL file";
        return false;
    }
    const uint32_t w = le32(d + 32), h = le32(d + 36), offset = le32(d + 40);
    if (w == 0 || h == 0 || w > 8192 || h > 8192)
    {
        error = "bad WAL size";
        return false;
    }
    if (offset < 100 || offset > size || size - offset < size_t(w) * h)
    {
        error = "WAL data is cut short";
        return false;
    }
    uint32_t palette[256];
    const std::string found = find_palette(path, {}, {"pics/colormap.pcx", "PICS/COLORMAP.PCX"}, true, palette);
    put_indexed(d + offset, int(w), int(h), found.empty() ? nullptr : palette, 255, out);
    out.format = "Quake 2 WAL";

    auto text = [&](size_t at, size_t len)
    {
        size_t n = 0;
        while (n < len && d[at + n])
            ++n;
        return std::string(reinterpret_cast<const char*>(d + at), n);
    };
    const std::string name = text(0, 32), next = text(56, 32);
    const uint32_t flags = le32(d + 88), contents = le32(d + 92), value = le32(d + 96);
    if (!name.empty())
        out.add(N_("WAL"), N_("Name"), name);
    note_palette("WAL", found, out);
    if (!next.empty())
        out.add(N_("WAL"), N_("Next frame"), next);
    static const char* flag_names[] = {"light", "slick", "sky", "warp", "trans33", "trans66", "flowing", "nodraw"};
    std::string list;
    for (int i = 0; i < 8; ++i)
        if (flags & 1u << i)
            list += (list.empty() ? "" : ", ") + std::string(flag_names[i]);
    if (flags & ~0xFFu)
        list += (list.empty() ? "" : ", ") + std::string("others");
    if (!list.empty())
        out.add(N_("WAL"), N_("Surface flags"), list);
    if (contents)
    {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%X", contents);
        out.add(N_("WAL"), N_("Contents"), hex);
    }
    if (value)
        out.add(N_("WAL"), N_("Light value"), std::to_string(value));
    return true;
}

bool decode_lmp(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size))
    {
        error = "cannot read LMP";
        return false;
    }
    const std::string name = base_name(path);
    uint32_t palette[256];
    auto palette_for = [&]() { return find_palette(path, {"palette.lmp", "PALETTE.LMP"}, {"gfx/palette.lmp", "GFX/PALETTE.LMP"}, false, palette); };

    // A qpic fills the file (some tools pad it to a word or so).
    const uint32_t w = size >= 8 ? le32(d) : 0, h = size >= 8 ? le32(d + 4) : 0;
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096 && size >= 8 + size_t(w) * h && size - (8 + size_t(w) * h) < 16)
    {
        const std::string found = palette_for();
        put_indexed(d + 8, int(w), int(h), found.empty() ? nullptr : palette, 255, out);
        out.format = "Quake LMP";
        note_palette("LMP", found, out);
        return true;
    }
    if (size == 768)
    {
        // The palette itself: one pixel a colour, 16 to a row.
        std::vector<uint8_t> bytes(d, d + size);
        palette_lmp(bytes, palette);
        out.allocate(16, 16);
        for (int i = 0; i < 256; ++i)
            out.row(i / 16)[i % 16] = palette[i];
        out.bits_per_sample = 8;
        out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, "RGB", 8);
        out.format = "Quake palette";
        out.add(N_("LMP"), N_("Contents"), "palette, 256 colours");
        return true;
    }
    // The lighting table: 64 shades of each colour, plus a stray byte.
    const bool colormap = (size == 16384 || size == 16385) && name.find("colormap") != std::string::npos;
    const bool conchars = size == 16384 && name.find("conchars") != std::string::npos;
    if (colormap || conchars)
    {
        const std::string found = palette_for();
        const int cw = colormap ? 256 : 128, ch = colormap ? 64 : 128;
        put_indexed(d, cw, ch, found.empty() ? nullptr : palette, conchars ? 0 : -1, out);
        out.format = "Quake LMP";
        out.add(N_("LMP"), N_("Contents"), colormap ? "colormap, 64 light levels" : "console font");
        note_palette("LMP", found, out);
        return true;
    }
    error = "not a Quake picture (no qpic header, and not a known lump)";
    return false;
}

}  // namespace

// No signatures: the extension alone claims these.
const Decoder& wal_decoder()
{
    static const Decoder d{"WAL", {"wal"}, nullptr, decode_wal};
    return d;
}

const Decoder& lmp_decoder()
{
    static const Decoder d{"LMP", {"lmp"}, nullptr, decode_lmp};
    return d;
}
