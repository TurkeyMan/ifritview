// The camera's JPEG registered against LibRaw's develop cropped to the camera
// inset (docs/camera-jpeg-viewport.md): residuals in raw pixels, CPU only.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// jpeglib.h needs FILE and size_t declared first.
#include <jpeglib.h>
#include <libraw/libraw.h>
#include <memory>
#include <vector>
struct RGB
{
    int w = 0, h = 0;
    std::vector<unsigned char> p;
};
RGB jpeg(const unsigned char* p, size_t n)
{
    jpeg_decompress_struct d{};
    jpeg_error_mgr e;
    d.err = jpeg_std_error(&e);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, p, n);
    jpeg_read_header(&d, TRUE);
    d.out_color_space = JCS_RGB;
    jpeg_start_decompress(&d);
    RGB out{int(d.output_width), int(d.output_height), {}};
    out.p.resize(size_t(out.w) * out.h * 3);
    while (d.output_scanline < d.output_height)
    {
        JSAMPROW row = out.p.data() + size_t(d.output_scanline) * out.w * 3;
        jpeg_read_scanlines(&d, &row, 1);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    return out;
}
std::vector<float> gray(const RGB& s, int w, int h)
{
    std::vector<float> g(size_t(w) * h);
    auto at = [&](int x, int y)
    {
        const auto* p = s.p.data() + (size_t(std::clamp(y, 0, s.h - 1)) * s.w + std::clamp(x, 0, s.w - 1)) * 3;
        return std::log(1.f + (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]));
    };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            double px = (x + .5) * s.w / w - .5, py = (y + .5) * s.h / h - .5;
            int ix = int(std::floor(px)), iy = int(std::floor(py));
            float a = float(px - ix), b = float(py - iy);
            g[size_t(y) * w + x] = (1 - b) * ((1 - a) * at(ix, iy) + a * at(ix + 1, iy)) + b * ((1 - a) * at(ix, iy + 1) + a * at(ix + 1, iy + 1));
        }
    // Same small blur on both samples; avoid a JPEG sharpened edge dominating the fit.
    auto out = g;
    for (int y = 1; y < h - 1; y++)
        for (int x = 1; x < w - 1; x++)
        {
            float sum = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    sum += g[size_t(y + dy) * w + x + dx] * (dx ? 1 : 2) * (dy ? 1 : 2);
            out[size_t(y) * w + x] = sum / 16;
        }
    return out;
}
struct G
{
    int w, h;
    std::vector<float> x, y;
};
G grad(const std::vector<float>& g, int w, int h)
{
    G out{w, h, std::vector<float>(g.size()), std::vector<float>(g.size())};
    for (int y = 1; y < h - 1; y++)
        for (int x = 1; x < w - 1; x++)
        {
            size_t i = size_t(y) * w + x;
            out.x[i] = g[i + 1] - g[i - 1];
            out.y[i] = g[i + w] - g[i - w];
        }
    return out;
}
double score(const G& a, const G& b, int dx, int dy, int x0, int y0, int x1, int y1)
{
    double aa = 0, bb = 0, ab = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            size_t i = size_t(y) * a.w + x, j = size_t(y + dy) * a.w + x + dx;
            aa += double(a.x[i]) * a.x[i] + double(a.y[i]) * a.y[i];
            bb += double(b.x[j]) * b.x[j] + double(b.y[j]) * b.y[j];
            ab += double(a.x[i]) * b.x[j] + double(a.y[i]) * b.y[j];
        }
    return aa * bb > 1e-15 ? ab / std::sqrt(aa * bb) : -1;
}
struct Fit
{
    double x, y, ncc;
};
Fit fit(const G& a, const G& b, int x0, int y0, int x1, int y1, int radius)
{
    int bx = 0, by = 0;
    double best = -2;
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
        {
            double s = score(a, b, dx, dy, x0, y0, x1, y1);
            if (s > best)
            {
                best = s;
                bx = dx;
                by = dy;
            }
        }
    auto sub = [](double m, double c, double p)
    {
        double den = m - 2 * c + p;
        return den < 0 ? std::clamp(.5 * (m - p) / den, -.5, .5) : 0.;
    };
    double sx = sub(score(a, b, bx - 1, by, x0, y0, x1, y1), best, score(a, b, bx + 1, by, x0, y0, x1, y1));
    double sy = sub(score(a, b, bx, by - 1, x0, y0, x1, y1), best, score(a, b, bx, by + 1, x0, y0, x1, y1));
    return {bx + sx, by + sy, best};
}
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int f = 1; f < argc; f++)
    {
        auto raw = std::make_unique<LibRaw>();
        int e = raw->open_file(argv[f]);
        if (e)
        {
            std::printf("FILE %s ERROR %s\n", argv[f], libraw_strerror(e));
            continue;
        }
        const auto& s = raw->imgdata.sizes;
        auto c = s.raw_inset_crops[0];
        int left = int(c.cleft) - s.left_margin, top = int(c.ctop) - s.top_margin;
        bool known = c.cwidth && c.cheight && c.cleft != 65535 && c.ctop != 65535 && left >= 0 && top >= 0 && left + int(c.cwidth) <= s.width &&
                     top + int(c.cheight) <= s.height;
        std::printf("FILE %s\n ACTIVE %ux%u MARGINS %u,%u CAMERA %d,%d,%u,%u VALID %d FLIP %d\n", argv[f], s.width, s.height, s.left_margin, s.top_margin, left,
                    top, c.cwidth, c.cheight, known, s.flip);
        if ((e = raw->unpack_thumb()))
        {
            std::printf(" ERROR thumb %s\n", libraw_strerror(e));
            continue;
        }
        auto* t = raw->dcraw_make_mem_thumb(&e);
        if (!t)
        {
            std::printf(" ERROR make thumb %d\n", e);
            continue;
        }
        RGB j;
        if (t->type == LIBRAW_IMAGE_JPEG)
            j = jpeg(t->data, t->data_size);
        else if (t->type == LIBRAW_IMAGE_BITMAP && t->colors == 3 && t->bits == 8)
        {
            j.w = t->width;
            j.h = t->height;
            j.p.assign(t->data, t->data + t->data_size);
        }
        LibRaw::dcraw_clear_mem(t);
        if (j.p.empty())
        {
            std::printf(" ERROR no RGB thumbnail\n");
            continue;
        }
        auto& p = raw->imgdata.params;
        p.use_camera_wb = 1;
        p.output_color = 1;
        p.output_bps = 8;
        p.user_qual = 0;
        p.no_auto_bright = 0;
        p.user_flip = 0;  // as the sensor has it, as the camera stores its JPEG (turned by EXIF)
        if (known)
        {
            p.cropbox[0] = left;
            p.cropbox[1] = top;
            p.cropbox[2] = c.cwidth;
            p.cropbox[3] = c.cheight;
        }
        if ((e = raw->unpack()) || (e = raw->dcraw_process()))
        {
            std::printf(" ERROR develop %s\n", libraw_strerror(e));
            continue;
        }
        auto* r = raw->dcraw_make_mem_image(&e);
        if (!r || r->type != LIBRAW_IMAGE_BITMAP || r->colors != 3 || r->bits != 8)
        {
            std::printf(" ERROR reference pixels %d\n", e);
            if (r)
                LibRaw::dcraw_clear_mem(r);
            continue;
        }
        RGB rgb{r->width, r->height, {}};
        rgb.p.assign(r->data, r->data + r->data_size);
        LibRaw::dcraw_clear_mem(r);
        int w = 1024, h = int(std::lround(double(rgb.h) * w / rgb.w));
        G a = grad(gray(rgb, w, h), w, h), b = grad(gray(j, w, h), w, h);
        Fit all = fit(a, b, 24, 24, w - 24, h - 24, 6);
        double ux = double(known ? c.cwidth : s.width) / w, uy = double(known ? c.cheight : s.height) / h;
        std::printf(" PIXELS raw=%dx%d jpeg=%dx%d grid=%dx%d\n GLOBAL raw-pixel residual=(%+.4f,%+.4f) NCC=%.6f grid-shift=(%+.4f,%+.4f)\n", rgb.w, rgb.h, j.w,
                    j.h, w, h, all.x * ux, all.y * uy, all.ncc, all.x, all.y);
        double worst = 0;
        int valid = 0;
        for (int yy = 0; yy < 3; yy++)
            for (int xx = 0; xx < 3; xx++)
            {
                int x0 = 24 + xx * (w - 48) / 3, y0 = 24 + yy * (h - 48) / 3, x1 = 24 + (xx + 1) * (w - 48) / 3, y1 = 24 + (yy + 1) * (h - 48) / 3;
                Fit v = fit(a, b, x0, y0, x1, y1, 5);
                double d = std::hypot(v.x * ux, v.y * uy);
                if (v.ncc > .6)
                {
                    worst = std::max(worst, d);
                    ++valid;
                }
                std::printf(" PATCH %d,%d residual=(%+.4f,%+.4f) NCC=%.6f\n", xx, yy, v.x * ux, v.y * uy, v.ncc);
            }
        std::printf(" SUMMARY valid-patches=%d max-raw-pixel-residual=%.4f authoritative=%d\n", valid, worst, known);
    }
}
