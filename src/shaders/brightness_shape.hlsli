// The brightness curve's shape (volume.hlsl's CURVE; --check-brightness on
// the CPU), after brightness_curve.hlsli. The includer declares
// g_count[kCurveBins] and g_floor[kCurveBins] (groupshared in HLSL) and
// CURVE_LOOP ([loop] in HLSL, nothing in C++).
// From g_count (pixels per bin, kCurveBins equal bins from `knee` to `top`),
// each bin's share of the output range into g_count: at least its floor, at
// most its own width (slope 1), and between, the rest in proportion to its
// count, the excess over any bin's cap clipped and redistributed to the
// others until none is left (the paper's iteration, solved here by halving
// for the proportion that leaves none). Ranges without pixels share what's
// left only if the counted ones are all at their caps. The shares add up to
// peak - knee.
void curve_shape(float knee, float top, float peak, float m)
{
    const float span = top - knee, range = peak - knee, width = span / float(kCurveBins);
    float floors = 0;
    CURVE_LOOP for (uint f = 0; f < kCurveBins; ++f)
    {
        g_floor[f] = curve_floor(f, width, m);
        floors += g_floor[f] * width;
    }
    const float rest = max(range - floors, 0.0f);  // (curve_knee() keeps it at or over 0)
    float counted_room = 0, high = 0;
    CURVE_LOOP for (uint r = 0; r < kCurveBins; ++r)
    {
        const float room = (1 - g_floor[r]) * width;
        if (g_count[r] > 0)
        {
            counted_room += room;
            high = max(high, room / g_count[r]);
        }
    }
    float share = high, spare = 0;
    if (counted_room > rest)
    {
        float least = 0;
        CURVE_LOOP for (int it = 0; it < 40; ++it)
        {
            const float mid = 0.5f * (least + share);
            float given = 0;
            CURVE_LOOP for (uint q = 0; q < kCurveBins; ++q)
                given += min(mid * g_count[q], (1 - g_floor[q]) * width);
            if (given > rest)
                share = mid;
            else
                least = mid;
        }
        share = least;  // the side that doesn't overfill: no floor is scaled under itself below
    }
    else
    {
        // The counted bins all at slope 1: what's left to the empty ones, by their room.
        float empty_room = 0;
        CURVE_LOOP for (uint u = 0; u < kCurveBins; ++u)
            empty_room += g_count[u] > 0 ? 0 : (1 - g_floor[u]) * width;
        spare = empty_room > 0 ? (rest - counted_room) / empty_room : 0;
    }
    float given = 0;
    CURVE_LOOP for (uint v = 0; v < kCurveBins; ++v)
    {
        const float room = (1 - g_floor[v]) * width;
        g_count[v] = g_floor[v] * width + (g_count[v] > 0 ? min(share * g_count[v], room) : spare * room);
        given += g_count[v];
    }
    // What halving leaves short of the range (a hair; or more only if every
    // bin is at its cap, which can't be with the top over the peak) to the
    // bins with room, by their room: the floors are kept, the caps too.
    const float short_by = range - given;
    float room_left = 0;
    CURVE_LOOP for (uint w = 0; w < kCurveBins; ++w)
        room_left += max(width - g_count[w], 0.0f);
    CURVE_LOOP for (uint x = 0; x < kCurveBins; ++x)
        g_count[x] += room_left > 0 && short_by > 0 ? short_by * max(width - g_count[x], 0.0f) / room_left : 0;
}
