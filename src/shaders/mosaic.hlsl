// The active area of a readout decoded on the GPU (ljpeg passes), as the
// develop pass takes a mosaic: a photosite a texel, 16-bit.
#include "ljpeg.hlsli"

ByteAddressBuffer values : register(t8);

float main(float4 position : SV_Position) : SV_Target
{
    const uint2 o = uint2(position.xy);
    const uint i = readout_index(o.x + left, o.y + top);
    return SAMPLE(values, i) / 65535.0;
}
