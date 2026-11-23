// Where each colour clips (clip.hlsl): its values counted over the top of
// the range, as a fraction of it from black to the stated white, from
// kClipFrom on in kClipBins steps to kClipFrom + kClipSpan; how many of each
// colour there are in all; then what clip.hlsl's FIND made of those, each
// colour's clip as such a fraction (a float), 0 where its values don't pile
// up at a top of their own. Then each colour's values over the developed
// image (develop.hlsl's PILE), counted in kPileBins steps of its level as a
// fraction of that clip from kPileFrom up; and what LIMIT made of those,
// each colour's linearity limit as such a fraction (a float; 1: none below
// its clip): the one each colour is censored at, its piles' first
// (kClipPiled, LIMIT's) and then, from COMBINE, the lower of those and its
// drift's. Then the drift (drift.hlsli): each colour's sums over kDriftBins
// levels (floats, six a level: kDriftSums) in each of the image's quadrants
// (kDriftGroups, the colours' sums a quadrant's), what COMBINE found of them (a
// limit each, as a fraction of the clip; and 1 where that's sure, 0 not),
// the limits remembered for the camera (gpu_view's, from raw_linearity.h;
// 0: none), and how many of PILE's values are past their colour's limit,
// under its clip (COMBINE's: a float, for the trace); last, whether any
// photosite at all is past its colour's limit (COMBINE's, from the counts
// over the mosaic: 1 or 0; 0, and the develop has nothing to estimate).
// Then the readout's noise, each colour's variance as the masked border
// has it (levels' differences), in the develop's balanced units (1 = where
// it clips), 0 where it wasn't measured.
#define kClipBins 2048u
#define kClipFrom 0.75f
#define kClipSpan 0.3f
#define kClipTotal (3u * kClipBins)
#define kClipFound (kClipTotal + 3u)
#define kClipWhite (kClipFound + 3u)  // float: the white the develop scales by (develop.hlsli's develop_white()), 1 = the stated white
#define kPileBins 192u
#define kPileFrom 0.25f
#define kClipPile (kClipWhite + 1u)
#define kClipLimit (kClipPile + 3u * kPileBins)
#define kClipPiled (kClipLimit + 3u)
#define kDriftBins 24u
#define kDriftSums 6u
#define kDriftGroups 4u  // the image's quadrants, each its own sums (drift.hlsli)
#define kClipDrift (kClipPiled + 3u)
#define kClipDrifted (kClipDrift + kDriftGroups * 3u * kDriftBins * kDriftSums)
#define kClipDriftSure (kClipDrifted + 3u)
#define kClipStored (kClipDriftSure + 3u)
#define kClipCensored (kClipStored + 3u)
#define kClipAny (kClipCensored + 1u)
#define kClipReadNoise (kClipAny + 1u)
#define kClipWords (kClipReadNoise + 3u)

// The black level's and the readout noise's measure where the sensor is
// masked (ljpeg_black's levels): sum R G B G2, count R G B G2, zeros; then
// the squared differences of photosites of a colour two rows apart, R G B
// G2 each in 64 bits (low word first), and how many of those.
#define kLevelsSquares 9u
#define kLevelsDifferences 17u
#define kLevelsWords 21u
