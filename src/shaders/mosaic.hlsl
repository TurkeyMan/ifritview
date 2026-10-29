// The active area of a readout decoded on the GPU (ljpeg passes), as the
// develop pass takes a mosaic: a photosite a texel, 16-bit; a linear DNG's
// three colours a photosite (LINEAR), into R, G, B.
#include "ljpeg.hlsli"

ByteAddressBuffer values : register(t8);

#ifdef LINEAR
float4 main(float4 position : SV_Position) : SV_Target
{
    const uint2 o = uint2(position.xy);
    const uint i = readout_index(o.x + left, o.y + top) * 3;
    return float4(SAMPLE(values, i), SAMPLE(values, i + 1), SAMPLE(values, i + 2), 65535) / 65535.0;
}
#else
float main(float4 position : SV_Position) : SV_Target
{
    const uint2 o = uint2(position.xy);
    const uint i = readout_index(o.x + left, o.y + top);
    return SAMPLE(values, i) / 65535.0;
}
#endif
