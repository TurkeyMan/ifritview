// The brightness curve's shape (view.hlsli's brightness(), volume.hlsl's
// CURVE), in arithmetic both HLSL and C++ read, so --check-brightness runs
// the same code on the CPU (with brightness_shape.hlsli). All luminances are
// log2. In C++ the includer declares min, max, log2 and uint.
//
// The knee is where the curve may first bend: kKnee of the peak, lower when
// the image's span above it is so wide that its floors (kMinBrightnessSlope,
// 1 at the knee and down to it over kKneeRamp stops) wouldn't fit in the
// display's range above it. The floors are never given up: the knee is moved
// instead, to the highest level at which they fit.
static const float kKnee = 0.5f, kKneeRamp = 0.25f;
static const uint kCurveBins = 128;

// The floor's slope, under 1 (at 1 nothing over the peak could fit).
float curve_least(float m)
{
    return min(max(m, 0.0f), 0.9f);
}

// The floor of bin f, `width` stops wide: at its lower edge (the floor falls
// with the input, so that edge's is the bin's most).
float curve_floor(uint f, float width, float m)
{
    return max(curve_least(m), 1 - float(f) * width / kKneeRamp);
}

// How far the floors over kCurveBins bins from `knee` to `top` overrun the
// range above the knee (peak - knee): fit if 0 or less. Lowering the knee a
// stop widens the range a stop and the floors less (their slope is under 1),
// so this falls as the knee does.
float curve_overrun(float knee, float top, float peak, float m)
{
    const float width = (top - knee) / float(kCurveBins);
    float floors = 0;
    for (uint f = 0; f < kCurveBins; ++f)
        floors += curve_floor(f, width, m) * width;
    return floors - (peak - knee);
}

// The knee for an image whose top is `top`, on a display whose peak is
// `peak`: kKnee of the peak if the floors fit under it, else the highest at
// which they do, found by halving between kKnee of the peak and a knee
// they're sure to fit under. Over a span S above the knee the floors take at
// most m S + kKneeRamp (1 - m)^2 / 2 (the ramp's area above m, the rest at
// m), and taken at each bin's lower edge at most (1 - m) S / kCurveBins more;
// solved for the knee at which that's the range, that's the sure one.
float curve_knee(float top, float peak, float m)
{
    const float least = curve_least(m), high = peak + log2(kKnee);
    if (curve_overrun(high, top, peak, m) <= 0)
        return high;
    const float rate = least + (1 - least) / float(kCurveBins), ramp = kKneeRamp * (1 - least) * (1 - least) / 2;
    float low = min(high, (peak - rate * top - ramp) / (1 - rate)), over = high;
    for (int i = 0; i < 24; ++i)
    {
        const float mid = 0.5f * (low + over);
        if (curve_overrun(mid, top, peak, m) <= 0)
            low = mid;
        else
            over = mid;
    }
    return low;
}
