// Perceived brightness, and the display model's hold on its order across
// hues (view.hlsli's tone_map()), in arithmetic both HLSL and C++ read, so
// --check-brightness runs the same code on the CPU. Colours are linear
// BT.709 RGB and CIE XYZ (D65), 1 = SDR white. In C++ the includer declares
// float3 (x, y, z; + - with float3, * / by float), dot, min, max, abs, sqrt,
// sin, atan2, exp2 and log2.
//
// The model: Fairchild and Pirrotta's Helmholtz-Kohlrausch-corrected
// lightness (1991),
//   L** = L* + (2.5 - 0.025 L*) (0.116 |sin((h - 90 deg) / 2)| + 0.085) C*
// from CIELAB L*, C* and hue h (white 1): a saturated colour looks brighter
// than a grey of the same luminance, by more the more chroma it has, and
// most for blues and purples, least for yellows. Over SDR white (L* > 100,
// an HDR display) the boost is held at none rather than made negative.

float cie_f(float t)
{
    return t > 0.008856f ? exp2(log2(t) / 3) : 7.787f * t + 16.0f / 116;
}

// CIELAB L*, a*, b* of `xyz`, white D65 at 1.
float3 cie_lab(float3 xyz)
{
    const float fx = cie_f(max(xyz.x, 0.0f) / 0.95047f), fy = cie_f(max(xyz.y, 0.0f)), fz = cie_f(max(xyz.z, 0.0f) / 1.08883f);
    return float3(116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz));
}

// The luminance of CIELAB lightness L.
float cie_y(float L)
{
    const float f = (L + 16) / 116;
    return f > 0.206893f ? f * f * f : (f - 16.0f / 116) / 7.787f;
}

float lightness_of(float y)
{
    return cie_lab(float3(0.95047f * y, y, 1.08883f * y)).x;
}

// Fairchild and Pirrotta's L** of `xyz`.
float perceived_lightness(float3 xyz)
{
    const float3 lab = cie_lab(xyz);
    const float c = sqrt(lab.y * lab.y + lab.z * lab.z), h = atan2(lab.z, lab.y);
    return lab.x + max(2.5f - 0.025f * lab.x, 0.0f) * (0.116f * abs(sin((h - 1.5707963f) / 2)) + 0.085f) * c;
}

float3 xyz_of_rgb709(float3 rgb)
{
    return float3(dot(float3(0.4124564f, 0.3575761f, 0.1804375f), rgb), dot(float3(0.2126729f, 0.7151522f, 0.0721750f), rgb),
                  dot(float3(0.0193339f, 0.1191920f, 0.9503041f), rgb));
}

// Oklab from and to linear BT.709 (as view.hlsli's oklab_from_rgb() and
// rgb_from_oklab(), in arithmetic C++ reads too).
float signed_cbrt(float v)
{
    return v == 0 ? 0 : (v > 0 ? 1 : -1) * exp2(log2(abs(v)) / 3);
}
float3 ok_from_rgb709(float3 c)
{
    const float l = signed_cbrt(dot(float3(0.4122214708f, 0.5363325363f, 0.0514459929f), c));
    const float m = signed_cbrt(dot(float3(0.2119034982f, 0.6806995451f, 0.1073969566f), c));
    const float s = signed_cbrt(dot(float3(0.0883024619f, 0.2817188376f, 0.6299787005f), c));
    const float3 lms = float3(l, m, s);
    return float3(dot(float3(0.2104542553f, 0.7936177850f, -0.0040720468f), lms), dot(float3(1.9779984951f, -2.4285922050f, 0.4505937099f), lms),
                  dot(float3(0.0259040371f, 0.7827717662f, -0.8086757660f), lms));
}
float3 rgb709_from_ok(float3 o)
{
    const float l = dot(float3(1, 0.3963377774f, 0.2158037573f), o), m = dot(float3(1, -0.1055613458f, -0.0638541728f), o),
                s = dot(float3(1, -0.0894841775f, -1.2914855480f), o);
    const float3 lms = float3(l * l * l, m * m * m, s * s * s);
    return float3(dot(float3(4.0767416621f, -3.3077115913f, 0.2309699292f), lms), dot(float3(-1.2684380046f, 2.6097574011f, -0.3413193965f), lms),
                  dot(float3(-0.0041960863f, -0.7034186147f, 1.7076147010f), lms));
}

bool fits_display(float3 rgb, float peak)
{
    return max(rgb.x, max(rgb.y, rgb.z)) <= peak && min(rgb.x, min(rgb.y, rgb.z)) >= 0;
}

// A colour the display can't show at luminance y (`rgb`, its hue's), towards
// white at y until every channel fits in 0..peak (view.hlsli's
// fit_display()), at constant Oklab hue, so it doesn't drift on its way to
// white as a straight line in linear RGB does (the Abney effect: oranges
// going yellow or pink, blues purple). Its Oklab chroma is scaled down at
// its Oklab lightness, then the whole colour scaled to luminance y: scaling
// linear RGB scales Oklab's L, a and b alike, so the hue is kept. The most
// chroma that fits is halved for, 16 times.
float3 at_luminance(float3 rgb, float y)
{
    const float has = dot(float3(0.2126729f, 0.7151522f, 0.0721750f), rgb);
    return has > 1e-12f ? rgb * (y / has) : float3(y, y, y);
}
float3 toward_white(float3 rgb, float y, float peak)
{
    if (fits_display(rgb, peak))
        return rgb;
    if (y >= peak)
        return float3(peak, peak, peak);
    if (y <= 0)
        return float3(0, 0, 0);
    const float3 o = ok_from_rgb709(rgb);
    float lo = 0, hi = 1;
    for (int i = 0; i < 16; ++i)
    {
        const float t = 0.5f * (lo + hi);
        if (fits_display(at_luminance(rgb709_from_ok(float3(o.x, o.y * t, o.z * t)), y), peak))
            lo = t;
        else
            hi = t;
    }
    return at_luminance(rgb709_from_ok(float3(o.x, o.y * lo, o.z * lo)), y);
}

// Perceived brightness matched: `rgb`, the colour fit's choice, shown with
// an L** of exactly `target` (what a grey of the same input L** comes out
// at), so output L** is one function of input L** for every hue and
// brightness order holds across hues by construction. The fit's Oklab
// chroma and hue (a, b) are kept and only its Oklab lightness is chosen:
// halved for 20 times between black and the peak's white, each try shown
// through toward_white() at its own luminance where it's past the display.
// L** is continuous along that path, from 0 at black to the peak white's,
// which no grey's output exceeds, so the halving always closes on target;
// chroma the display can't show at that lightness goes towards white at
// constant hue. kLightnessChromaWeight's choice of chroma is kept wherever
// it fits at the matched lightness.
float3 shown_at(float L, float a, float b, float peak)
{
    const float3 c = rgb709_from_ok(float3(L, a, b));
    return toward_white(c, dot(float3(0.2126729f, 0.7151522f, 0.0721750f), c), peak);
}
float3 match_perceived(float3 rgb, float target, float peak)
{
    if (!(target > 0))
        return float3(0, 0, 0);
    const float3 o = ok_from_rgb709(rgb);
    // From black to a lightness whose colour is past the peak's luminance,
    // so shown as the peak's white (the peak white's own, doubled while the
    // chroma holds the luminance under it).
    float lo = 0, hi = exp2(log2(peak) / 3);
    for (int k = 0; k < 8 && dot(float3(0.2126729f, 0.7151522f, 0.0721750f), rgb709_from_ok(float3(hi, o.y, o.z))) < peak; ++k)
        hi *= 2;
    for (int i = 0; i < 20; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        if (perceived_lightness(xyz_of_rgb709(shown_at(mid, o.y, o.z, peak))) >= target)
            hi = mid;
        else
            lo = mid;
    }
    return shown_at(hi, o.y, o.z, peak);
}
