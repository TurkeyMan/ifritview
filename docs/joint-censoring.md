# Colours that roll off together

## Cause

On a Canon 5D Mark III mRAW lit by a flame (_T0B7734), the flame's edge rendered pink (Oklab hue about 19°) inside a glow at about 70°. At the edge, red and green have both rolled off before encoding: red stays near 0.92 of the stated white and green near 0.93, while blue, still linear, rises 80%. With the store as it was, the camera's remembered red limit (0.84 of its clip) censored red, but green had no limit, either measured or remembered. Its stalled reading counted as a measurement. The fit then took the magnitude from green and blue and raised red alone from the colour field, so red came out about 1.3× too high against green.

Green has no limit of its own in that image. Its roll-off leaves no pile (it rolls off; it doesn't stop), and the flame's edge has too few smooth runs for the drift. The model's answer is the per-camera memory: limits seen on other images, kept for images that don't show their own. raw_linearity.h states this camera's green rolls off from about 0.85. Master's drift does find that limit, on _T0B7488 (0.844) and _T0B7619 (0.875), where green is the nearest its clip. Once one of those has been seen, green is censored on _T0B7734 too, and its edge takes the glow's hue. The pink came from a store that had never seen green's roll-off.

## Model rule

model.md §1: each channel's linearity limit is measured per channel from its own drift, and confident limits are remembered per camera and format. A reading past its limit is only a lower bound.

The drift read only one colour a run: the nearest its clip, against the other two's mean rise. A colour that rolls off together with the nearest, as an sRAW's red and green do, was never read. Its limit could be learned only from scenes where it happened to be the nearest. A run is evidence for any colour whose rise can be compared with the rise of colours further from their clips, which are still linear wherever that colour is.

## Change

`drift_run()` now reads the middle colour as well, against the furthest colour's rise alone, at its own level, with that colour's noise as the noise of d. The nearest colour is read as before (against the other two), so every limit found before is unchanged. The furthest colour has nothing to be read against. COMBINE, the store, GATHER (which already leaves censored values out of the colour field) and `estimate_colours()` (which already holds each censored colour at its bound) are unchanged.

`--check-highlight` adds a ramp whose red and green roll off together from 0.85, with blue linear: both limits are found (master finds red's only).

## What it does not do

It doesn't detect green's roll-off on _T0B7734 alone: that image has too few runs at those levels for either colour. Over the twelve 5D Mark III mRAWs and four Bayer and X-Trans raws checked, it finds no limit that master doesn't, so no output changes for a given store. It widens the evidence the store can learn from: scenes where green rolls off with red, not only those where green is the nearest.
