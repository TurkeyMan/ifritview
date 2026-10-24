// Netpbm: PBM (1-bit), PGM (grey), PPM (RGB), each plain (P1-P3, text) or
// raw (P4-P6), and PAM (P7: a header of keywords, then grey, RGB or either
// with alpha). Samples go up to 16 bits (a maxval past 255 is two bytes,
// big-endian) and are scaled to 8. A file can hold several images; the
// first is shown. ".pbm" is also Deluxe Paint's chunky IFF, which the ILBM
// decoder takes: the two are told apart by their first bytes.
#include "decoder.h"
#include "../i18n.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 3 && h[0] == 'P' && h[1] >= '1' && h[1] <= '7' && std::isspace(h[2]);
}

// A header (or plain raster) read token by token, skipping comments.
struct Reader
{
    const uint8_t* d;
    size_t n, pos = 0;

    void skip_space()
    {
        while (pos < n)
        {
            if (d[pos] == '#')
                while (pos < n && d[pos] != '\n' && d[pos] != '\r')
                    ++pos;
            else if (std::isspace(d[pos]))
                ++pos;
            else
                break;
        }
    }
    bool number(long& v)
    {
        skip_space();
        if (pos >= n || !std::isdigit(d[pos]))
            return false;
        v = 0;
        while (pos < n && std::isdigit(d[pos]))
        {
            v = v * 10 + (d[pos++] - '0');
            if (v > 0x7FFFFFFF)
                return false;
        }
        return true;
    }
    std::string word()
    {
        skip_space();
        std::string w;
        while (pos < n && !std::isspace(d[pos]))
            w += char(d[pos++]);
        return w;
    }
    // A plain PBM's pixels are single digits, with or without space between.
    bool bit(int& b)
    {
        skip_space();
        if (pos >= n || (d[pos] != '0' && d[pos] != '1'))
            return false;
        b = d[pos++] - '0';
        return true;
    }
};

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!file_bytes(path, data, size) || !sniff(data, size))
    {
        error = "not a Netpbm file";
        return false;
    }
    const int kind = data[1] - '0';
    Reader r{data, size, 2};
    long w = 0, h = 0, maxval = 1, depth = 1;
    std::string tupltype;
    if (kind == 7)
    {
        // PAM: KEYWORD value lines up to ENDHDR.
        for (;;)
        {
            const std::string key = r.word();
            if (key.empty())
            {
                error = "PAM header has no ENDHDR";
                return false;
            }
            if (key == "ENDHDR")
                break;
            if (key == "TUPLTYPE")
            {
                // The rest of the line, which may hold spaces.
                while (r.pos < size && (data[r.pos] == ' ' || data[r.pos] == '\t'))
                    ++r.pos;
                while (r.pos < size && data[r.pos] != '\n' && data[r.pos] != '\r')
                    tupltype += char(data[r.pos++]);
                continue;
            }
            long v = 0;
            if (!r.number(v))
            {
                error = "bad PAM header";
                return false;
            }
            if (key == "WIDTH")
                w = v;
            else if (key == "HEIGHT")
                h = v;
            else if (key == "DEPTH")
                depth = v;
            else if (key == "MAXVAL")
                maxval = v;
        }
    }
    else
    {
        if (!r.number(w) || !r.number(h) || (kind != 1 && kind != 4 && !r.number(maxval)))
        {
            error = "bad Netpbm header";
            return false;
        }
        depth = kind == 3 || kind == 6 ? 3 : 1;
    }
    if (!Image::fits(w, h) || maxval < 1 || maxval > 65535 || depth < 1 || depth > 4)
    {
        error = "bad Netpbm header";
        return false;
    }
    const bool bitmap = kind == 1 || kind == 4 || tupltype.rfind("BLACKANDWHITE", 0) == 0;
    const bool alpha = depth == 2 || depth == 4;
    const bool colour = depth >= 3;
    // A raw raster starts after ENDHDR's line (PAM) or one whitespace byte;
    // a plain one is read as tokens from here.
    const bool raw = kind >= 4;
    if (kind == 7)
    {
        while (r.pos < size && data[r.pos] != '\n')
            ++r.pos;
        ++r.pos;
    }
    else if (raw)
        ++r.pos;

    const int bytes_per_sample = maxval > 255 ? 2 : 1;
    const size_t row_bytes = kind == 4 ? size_t(w + 7) / 8 : size_t(w) * depth * bytes_per_sample;
    if (raw && (r.pos > size || size - r.pos < row_bytes * size_t(h)))
    {
        error = "Netpbm file is cut short";
        return false;
    }

    out.allocate(int(w), int(h));
    const auto scale = [maxval](long v) { return uint32_t(std::min(v, maxval) * 255 / maxval); };
    long sample[4] = {0, 0, 0, 0};
    for (long y = 0; y < h; ++y)
    {
        uint32_t* px = out.row(int(y));
        const uint8_t* row = data + r.pos + size_t(y) * row_bytes;
        for (long x = 0; x < w; ++x)
        {
            if (kind == 4)
                sample[0] = (row[x >> 3] >> (7 - (x & 7))) & 1;
            else if (kind == 1)
            {
                int b = 0;
                if (!r.bit(b))
                {
                    error = "Netpbm file is cut short";
                    return false;
                }
                sample[0] = b;
            }
            else
                for (long c = 0; c < depth; ++c)
                {
                    if (raw)
                    {
                        const uint8_t* s = row + (size_t(x) * depth + c) * bytes_per_sample;
                        sample[c] = bytes_per_sample == 2 ? (s[0] << 8) | s[1] : s[0];
                    }
                    else if (!r.number(sample[c]))
                    {
                        error = "Netpbm file is cut short";
                        return false;
                    }
                }
            uint32_t red, green, blue, a = 255;
            if (kind == 1 || kind == 4)
                red = green = blue = sample[0] ? 0 : 255;  // in PBM, 1 is black
            else if (colour)
            {
                red = scale(sample[0]);
                green = scale(sample[1]);
                blue = scale(sample[2]);
            }
            else
                red = green = blue = scale(sample[0]);
            if (alpha)
            {
                a = scale(sample[depth - 1]);
                red = red * a / 255;  // stored premultiplied
                green = green * a / 255;
                blue = blue * a / 255;
            }
            px[x] = a << 24 | red << 16 | green << 8 | blue;
        }
    }
    out.has_alpha = alpha;
    const int bits = bitmap ? 1 : maxval > 255 ? 16 : 8;
    out.bits_per_sample = bits;
    static const char* const names[] = {"", "PBM", "PGM", "PPM", "PBM", "PGM", "PPM", "PAM"};
    out.format = names[kind];
    using M = PixelFormat::Model;
    out.pixel_format = PixelFormat::packed(colour ? M::RGB : M::L, colour ? (alpha ? "RGBA" : "RGB") : (alpha ? "LA" : "L"), bits);
    out.pixel_format.notes.push_back(raw ? "binary" : "plain text");
    if (!bitmap && maxval != 255 && maxval != 65535)
        out.add(N_("Netpbm"), N_("Maximum value"), std::to_string(maxval));
    if (!tupltype.empty())
        out.add(N_("Netpbm"), N_("Tuple type"), tupltype);
    return true;
}

}  // namespace

const Decoder& netpbm_decoder()
{
    static const Decoder d{"Netpbm", {"pbm", "pgm", "ppm", "pnm", "pam"}, sniff, decode};
    return d;
}
