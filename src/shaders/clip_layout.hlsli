// Where each colour clips (clip.hlsl): its values counted over the top of
// the range, as a fraction of it from black to the stated white, from
// kClipFrom on in kClipBins steps to kClipFrom + kClipSpan; how many of each
// colour there are in all; then what clip.hlsl's FIND made of those, each
// colour's clip as such a fraction (a float), 0 where its values don't pile
// up at a top of their own.
#define kClipBins 2048u
#define kClipFrom 0.75f
#define kClipSpan 0.3f
#define kClipTotal (3u * kClipBins)
#define kClipFound (kClipTotal + 3u)
#define kClipWords (kClipFound + 3u)
