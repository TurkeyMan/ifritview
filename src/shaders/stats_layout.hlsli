// Word offsets shared by the GPU statistics passes and their CPU readback.
#define kStatsLuminance 0u  // 1024 counts: log2 luminance, 2^-20 .. 2^12
#define kStatsExtents 1024u // 72 floats (as uints): how far from white per 5 degrees of hue, 1 = the display's edge
#define kStatsCount 1096u   // pixels counted
#define kStatsPeak 1097u    // float: the 99.9th percentile's luminance (peak.hlsl)
#define kStatsMinimum 1098u // ~float bits of the least luminance above 0 (max of the complement: a min)
#define kStatsMaximum 1099u // float bits of the greatest
#define kStatsP99 1100u     // float: the 99th percentile's luminance (peak.hlsl), for auto exposure
#define kStatsAutoGain 1101u // float: a raw's automatic exposure (peak.hlsl)
#define kStatsOpaque 1102u  // pixels counted at all, black ones too (stats.hlsl), for a raw's key
#define kStatsTop 1103u      // float: a raw's true maximum as edited, at full resolution, its few brightest pixels aside (volume.hlsl's CURVE)
#define kStatsVolumeRaw 1104u  // 72 hues x 32 lightness slices x 2 words: a raw's colours against the display's edge there, largest and second largest (volume.hlsl)
#define kStatsVolume 5712u     // 72 x 32 floats: that table's second largest, spread to neighbouring cells and smoothed (volume.hlsl's PREPARE)
#define kStatsFloor 8016u      // 72 x 32 floats: the least output lightness each cell may map to (volume.hlsl's PREPARE)
#define kStatsTopCounts 10320u // 1024 counts: full-resolution log2 luminance as edited, from an eighth of the level's greatest, 128 a stop (stats.hlsl's TOP)
#define kStatsTopMost 11344u   // float bits of the greatest luminance as edited at full resolution (stats.hlsl's TOP)
#define kStatsCurve 11345u     // a raw's brightness curve (volume.hlsl's CLEAR): log2 of its knee and of its top, then 129 log2 outputs at its bins' edges
#define kStatsCurveLuminance 11476u // 1024 counts: log2 luminance as edited (exposure, white balance, develop), 2^-20 .. 2^12 (stats.hlsl's CURVE)
#define kStatsCurveMost 12500u // float bits of the greatest of those (stats.hlsl's CURVE)
#define kStatsWords 12501u
