// Word offsets shared by the GPU statistics passes and their CPU readback.
#define kStatsLuminance 0u  // 1024 counts: log2 luminance, 2^-20 .. 2^12
#define kStatsExtents 1024u // 72 floats (as uints): how far from white per 5 degrees of hue, 1 = the display's edge
#define kStatsCount 1096u   // pixels counted
#define kStatsPeak 1097u    // float: the 99.9th percentile's luminance (peak.hlsl)
#define kStatsMinimum 1098u // ~float bits of the least luminance above 0 (max of the complement: a min)
#define kStatsMaximum 1099u // float bits of the greatest
#define kStatsP99 1100u     // float: the 99th percentile's luminance (peak.hlsl), for auto exposure
#define kStatsWords 1101u
