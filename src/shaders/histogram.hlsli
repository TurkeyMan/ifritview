// Drawing histogram.hlsl's counts: the overlay in the view's frame
// (compose.hlsl) and the docked panel (histogram_view.hlsl).
ByteAddressBuffer histogram : register(t9);  // R, G, B, Y counts (256 each), then the tallest

// The classic histogram at a point of `panel` (x0, y0, x1, y1): a dark backdrop, R, G and
// B as additive fills (where they overlap: cyan, magenta, yellow, white),
// and luminance as a light outline over them. Heights scale to the tallest
// bin between the ends (0 and 255 hold every clipped pixel and would flatten
// the rest); bins past the panel's top are cut. sRGB-encoded colour, alpha.
float4 histogram_panel(float2 at, float4 panel)
{
    const float2 size = panel.zw - panel.xy;
    const float x = (at.x - panel.x) / size.x, y = 1 - (at.y - panel.y) / size.y;  // 0..1, y up
    const uint bin = min(uint(x * 256), 255u);
    const float tallest = max(1.0, float(histogram.Load(1024 * 4)));
    float3 fill = 0;
    [unroll] for (uint ch = 0; ch < 3; ++ch)
    {
        const float h = float(histogram.Load((ch * 256 + bin) * 4)) / tallest;
        fill[ch] = y <= h ? 0.85 : 0;
    }
    const float luma = float(histogram.Load((768 + bin) * 4)) / tallest;
    const float px = 1 / size.y;  // a device pixel, in panel heights
    const bool outline = abs(y - min(luma, 1.0)) <= 1.5 * px && luma > 0;
    const float4 backdrop = float4(0.08, 0.08, 0.08, 0.7);
    if (outline)
        return float4(0.95, 0.95, 0.95, 0.95);
    if (any(fill > 0))
        return float4(max(fill, 0.08), 0.9);
    return backdrop;
}

