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
#define kStatsWhite 1103u    // float: a raw's white point for its highlight shoulder (peak.hlsl)
#define kStatsBrightest 1104u // 1024 counts: log2 of each pixel's largest of R, G, B (BT.709), as kStatsLuminance
#define kStatsWords 2128u
