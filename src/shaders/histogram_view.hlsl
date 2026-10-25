// The docked histogram panel: the whole target is the panel, opaque.
#include "histogram.hlsli"

cbuffer Params : register(b0)
{
    float4 panel;  // 0, 0, width, height
};

float4 main(float4 position : SV_Position) : SV_Target
{
    const float4 h = histogram_panel(position.xy, panel);
    return float4(h.rgb * h.a + 0.08 * (1 - h.a), 1);  // over the same dark the backdrop has
}
