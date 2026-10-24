#pragma once

#include <string>
#include <vector>

// How a file stores its pixels, as a structure rather than a label, so any
// encoding can be described the same way: RGBA5551, YCbCr 4:2:0, a 14-bit
// Bayer mosaic, float16 RGB, an 8-bit palette.
struct PixelFormat
{
    enum class Model
    {
        Unknown,
        L,  // grey: luminance (with or without alpha)
        RGB,
        YCbCr,
        CMYK,
        Lab,
        Palette,
        Mosaic,
        Vector
    };
    enum class Sample
    {
        Unsigned,
        Signed,
        Float
    };

    struct Channel
    {
        char name;  // 'R','G','B','A','L' (grey, or Lab's lightness),'Y','U','V','C','M','K','a','b','I' (index), 'X' (padding)
        int bits;
    };

    Model model = Model::Unknown;
    std::vector<Channel> channels;  // in storage order
    Sample sample = Sample::Unsigned;
    int bits_per_pixel = 0;  // packed size per pixel; 0 = not packed / unknown
    bool planar = false;
    bool premultiplied = false;
    int chroma_h = 1, chroma_v = 1;  // YCbCr subsampling factors
    std::string mosaic;              // "RGGB", "X-Trans"
    std::vector<std::string> notes;  // "RLE", "lossless", "256 entries", "transparency key"

    bool empty() const { return model == Model::Unknown && channels.empty(); }

    // Uniform-depth interleaved channels, e.g. rgb("RGBA", 8).
    static PixelFormat packed(Model model, const char* names, int bits_each)
    {
        PixelFormat f;
        f.model = model;
        for (const char* c = names; *c; ++c)
            f.channels.push_back({*c, bits_each});
        f.bits_per_pixel = bits_each * int(f.channels.size());
        return f;
    }
    // Per-channel widths, e.g. widths(RGB, "RGBA", {5,5,5,1}).
    static PixelFormat widths(Model model, const char* names, std::vector<int> bits, int bpp = 0)
    {
        PixelFormat f;
        f.model = model;
        size_t i = 0;
        for (const char* c = names; *c && i < bits.size(); ++c, ++i)
            f.channels.push_back({*c, bits[i]});
        f.bits_per_pixel = bpp;
        if (!bpp)
            for (const Channel& ch : f.channels)
                f.bits_per_pixel += ch.bits;
        return f;
    }
    static PixelFormat ycbcr(int bits, int h, int v)
    {
        PixelFormat f = packed(Model::YCbCr, "YUV", bits);
        f.chroma_h = h;
        f.chroma_v = v;
        f.planar = true;
        f.bits_per_pixel = 0;
        return f;
    }
    static PixelFormat palette(int index_bits, int entries = 0)
    {
        PixelFormat f;
        f.model = Model::Palette;
        f.channels.push_back({'I', index_bits});
        f.bits_per_pixel = index_bits;
        if (entries)
            f.notes.push_back(std::to_string(entries) + " entries");
        return f;
    }
    static PixelFormat mosaic_(const std::string& pattern, int bits)
    {
        PixelFormat f;
        f.model = Model::Mosaic;
        f.channels.push_back({'X', bits});
        f.bits_per_pixel = bits;
        f.mosaic = pattern;
        return f;
    }

    // "RGBA 5-5-5-1, 16 bpp" · "YCbCr 4:2:0 (chroma 160×100), 8 bpc" ·
    // "X-Trans mosaic, 14-bit" · "Palette 8-bit index, 256 entries" · "RGB float16"
    std::string describe(int width = 0, int height = 0) const
    {
        if (model == Model::Vector)
            return join_notes("vector");
        if (empty())
            return "";
        std::string s;
        if (model == Model::Mosaic)
        {
            s = mosaic.empty() ? "mosaic" : mosaic + " mosaic";
            s += ", " + std::to_string(channels.empty() ? bits_per_pixel : channels[0].bits) + "-bit";
            return join_notes(s);
        }
        if (model == Model::Palette)
        {
            s = "Palette " + std::to_string(bits_per_pixel) + "-bit index";
            return join_notes(s);
        }
        // Channel names, e.g. "RGBA"; YCbCr is spelt out.
        if (model == Model::YCbCr)
        {
            s = "YCbCr";
            if (chroma_h > 1 || chroma_v > 1)
            {
                s += std::string(" ") + (chroma_h == 2 && chroma_v == 2 ? "4:2:0" : chroma_h == 2 ? "4:2:2" : chroma_v == 2 ? "4:4:0" : "subsampled");
                if (width && height)
                    s += " (chroma " + std::to_string((width + chroma_h - 1) / chroma_h) + "×" + std::to_string((height + chroma_v - 1) / chroma_v) + ")";
            }
            else
            {
                s += " 4:4:4";
            }
            for (const Channel& c : channels)
                if (c.name == 'A')
                    s += " + alpha";
        }
        else if (channels.empty())
        {
            s = model == Model::L ? "Grey" : model == Model::RGB ? "RGB" : model == Model::CMYK ? "CMYK" : model == Model::Lab ? "Lab" : "";
        }
        else
        {
            for (const Channel& c : channels)
                s += c.name;
        }
        // Depth: uniform -> "8 bpc"; mixed -> "5-6-5"; floats -> "float16".
        bool uniform = true;
        for (const Channel& c : channels)
            uniform &= c.bits == channels[0].bits;
        const int bits0 = channels.empty() ? 0 : channels[0].bits;
        if (channels.empty())
        {
            // Nothing known per channel (a block-compressed texture): no depth to give.
        }
        else if (sample == Sample::Float)
        {
            s += " float" + std::to_string(bits0);
        }
        else if (uniform)
        {
            if (model != Model::YCbCr)
                s += " " + std::to_string(bits0) + " bpc";
            else
                s += ", " + std::to_string(bits0) + " bpc";
            if (sample == Sample::Signed)
                s += " signed";
        }
        else
        {
            s += " ";
            for (size_t i = 0; i < channels.size(); ++i)
                s += (i ? "-" : "") + std::to_string(channels[i].bits);
        }
        if (bits_per_pixel && !uniform)
            s += ", " + std::to_string(bits_per_pixel) + " bpp";
        else if (bits_per_pixel && model != Model::YCbCr && sample != Sample::Float && bits_per_pixel != bits0 * int(channels.size()))
            s += ", " + std::to_string(bits_per_pixel) + " bpp";
        if (planar && model != Model::YCbCr)
            s += ", planar";
        if (premultiplied)
            s += ", premultiplied";
        return join_notes(s);
    }

private:
    std::string join_notes(std::string s) const
    {
        for (const std::string& n : notes)
            s += (s.empty() ? "" : ", ") + n;
        return s;
    }
};
