// One axis of a resampling filter, shared by the view's passes and the mip
// chain. Everything here is linear light, premultiplied.
//
// Output pixel `o` (centre o + 0.5) sits at source position
//   u = (o + 0.5) * inv + offset
// in texel units (texel i spans [i, i + 1]). `inv` is source texels per
// output pixel; past 1 (shrinking) the kernel is widened to match, so it
// filters out what the smaller image can't hold.

#define FILTER_NEAREST 0
#define FILTER_AREA 1
#define FILTER_MITCHELL 2
#define FILTER_CATMULL_ROM 3
#define FILTER_LANCZOS3 4

static const float kPi = 3.14159265358979;

float cubic(float x, float B, float C)
{
    x = abs(x);
    if (x < 1)
        return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
    if (x < 2)
        return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
    return 0;
}

float sinc(float x)
{
    x *= kPi;
    return abs(x) < 1e-5 ? 1.0 : sin(x) / x;
}

float support(int filter)
{
    return filter == FILTER_LANCZOS3 ? 3.0 : filter == FILTER_AREA ? 0.5 : 2.0;
}

// The weight of the texel spanning [t, t + 1] for the output pixel at u.
float weight(int filter, float t, float u, float inv)
{
    const float scale = max(inv, 1.0);  // the kernel's width in texels
    if (filter == FILTER_AREA)
    {
        // How much of the texel the output pixel's footprint covers.
        const float half_width = 0.5 * inv;
        return max(0.0, min(t + 1, u + half_width) - max(t, u - half_width));
    }
    const float x = (t + 0.5 - u) / scale;
    if (filter == FILTER_MITCHELL)
        return cubic(x, 1.0 / 3, 1.0 / 3);
    if (filter == FILTER_CATMULL_ROM)
        return cubic(x, 0, 0.5);
    return abs(x) < 3 ? sinc(x) * sinc(x / 3) : 0;  // Lanczos-3
}

// The first texel and how many the kernel touches at u (at most 32).
void taps(int filter, float u, float inv, out int first, out int count)
{
    const float reach = support(filter) * max(inv, 1.0);
    first = int(floor(u - reach));
    count = min(32, int(ceil(u + reach)) - first);
}
