#!/bin/sh
# Check a release build before packaging. Arguments: build directory and scratch directory.
set -eu
build=$1
smoke=$2

# Every shader valid for Vulkan: an invalid compute pipeline can hang a GPU.
for spv in "$build"/shaders/*.spv; do
  spirv-val --target-env vulkan1.3 "$spv"
done

# Decode without a display.
mkdir -p "$smoke"
printf 'P6\n2 2\n255\n\377\0\0\0\377\0\0\0\377\377\377\377' > "$smoke"/rgb.ppm
"$build/ifritview" --probe "$smoke"/rgb.ppm
"$build/ifritview" --version
if "$build/ifritview" --check-measures; then
  :
else
  result=$?
  # Some CI containers have no Vulkan device (including software). A
  # prerequisite skip is explicit; any actual numerical failure is fatal.
  [ "$result" = 77 ] || exit "$result"
fi
# The viewer's downscaler, which draws every zoomed-out image (GCC 16.2's
# LTO once miscompiled it): a 64x48 image to 23x17.
printf 'P6\n64 48\n255\n' > "$smoke"/big.ppm
head -c $((64 * 48 * 3)) /dev/urandom >> "$smoke"/big.ppm
"$build/ifritview" --process "$smoke"/big.ppm shrink:23,17 info
# The overlay's encoding line, as info prints it: a PNG's format and depth,
# nothing said of its loss (always lossless) or its space (untagged).
"$build/ifritview" --process "$smoke"/rgb.ppm save:"$smoke"/rgb.png
"$build/ifritview" --process "$smoke"/rgb.png info | grep -q '^encoding: PNG  ·  8-bit$'
# SVG: drawn 2048 px on its longer side (the view box alone gives the
# shape), gzipped too; a broken one refused, not crashed on (1 is a refusal).
svg=$(dirname "$0")/../../tools/svg
"$build/ifritview" --process "$svg"/viewbox.svg info | grep -q '^2048 x 512, SVG'
gzip -c "$svg"/shapes.svg > "$smoke"/shapes.svgz
"$build/ifritview" --process "$smoke"/shapes.svgz info | grep -q '^2048 x 1229, SVGZ, alpha'
"$build/ifritview" --process "$svg"/broken.svg info || [ $? = 1 ]
# References written loosely (spaces around '=', entities, '>' in a value),
# which the check against runaway <use>s reads as lunasvg does.
"$build/ifritview" --process "$svg"/references.svg info | grep -q '^2048 x 683, SVG'
# Text is paid for in every copy lunasvg makes of it: text-copies.svg's
# elements come to some 32 KB of the budget and its seventy copies of 1 KB
# of text to 72 KB more, so it's drawn, but refused under a 64 KB budget.
"$build/ifritview" --process "$svg"/text-copies.svg info | grep -q '^2048 x 512, SVG'
if IFRIT_SVG_BUDGET=65536 "$build/ifritview" --process "$svg"/text-copies.svg info 2>/dev/null | grep -q '^2048 x'; then
  echo "text-copies.svg was drawn past its budget" >&2
  exit 1
fi
# A GIMP XCF, the same four pixels as rgb.ppm: version 11 (64-bit
# pointers), one RGBA layer in one uncompressed tile.
printf 'gimp xcf v011\0\0\0\0\2\0\0\0\2\0\0\0\0\0\0\0\226\0\0\0\021\0\0\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\107\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0' > "$smoke"/rgb.xcf
printf '\0\0\0\2\0\0\0\2\0\0\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\157\0\0\0\0\0\0\0\0' >> "$smoke"/rgb.xcf
printf '\0\0\0\2\0\0\0\2\0\0\0\4\0\0\0\0\0\0\0\213\0\0\0\0\0\0\0\0\0\0\0\2\0\0\0\2\0\0\0\0\0\0\0\243\0\0\0\0\0\0\0\0' >> "$smoke"/rgb.xcf
printf '\377\0\0\377\0\377\0\377\0\0\377\377\377\377\377\377' >> "$smoke"/rgb.xcf
"$build/ifritview" --process "$smoke"/rgb.xcf diff:"$smoke"/rgb.ppm | grep -q '^identical'
# The same with its composite mode and space INT32_MIN, values GIMP
# doesn't have: shown as Normal all the same.
printf 'gimp xcf v011\0\0\0\0\2\0\0\0\2\0\0\0\0\0\0\0\226\0\0\0\021\0\0\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\107\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0' > "$smoke"/odd.xcf
printf '\0\0\0\2\0\0\0\2\0\0\0\1\0\0\0\0\0\0\0\043\0\0\0\4\200\0\0\0\0\0\0\044\0\0\0\4\200\0\0\0' >> "$smoke"/odd.xcf
printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\207\0\0\0\0\0\0\0\0' >> "$smoke"/odd.xcf
printf '\0\0\0\2\0\0\0\2\0\0\0\4\0\0\0\0\0\0\0\243\0\0\0\0\0\0\0\0\0\0\0\2\0\0\0\2\0\0\0\0\0\0\0\273\0\0\0\0\0\0\0\0' >> "$smoke"/odd.xcf
printf '\377\0\0\377\0\377\0\377\0\0\377\377\377\377\377\377' >> "$smoke"/odd.xcf
"$build/ifritview" --process "$smoke"/odd.xcf diff:"$smoke"/rgb.ppm | grep -q '^identical'
# A raw still being copied: a CR2 whose strip runs past the end of the file,
# and one with nothing written yet, are refused as not all there (read again
# when they're done), not shown as what another decoder makes of them.
printf 'II*\000\020\000\000\000CR\002\000\000\000\000\000\003\000\000\001\003\000\001\000\000\000\100\000\000\000\021\001\004\000\001\000\000\000\072\000\000\000\027\001\004\000\001\000\000\000\000\000\001\000\000\000\000\000' > "$smoke"/cut.cr2
: > "$smoke"/empty.cr2
for raw in cut empty; do
  "$build/ifritview" --probe "$smoke"/$raw.cr2 | grep -q "still being copied"
done
# A raw's brightness curve: never flatter than its least slope nor steeper
# than the scene, its top on the peak, however far over the peak it reaches.
"$build/ifritview" --check-brightness
# A raw's clipped colours estimated: two, one or no colours measured, the
# clip's bound winning or not, and one mixed (estimate.hlsli, as the GPU
# develop has it).
"$build/ifritview" --check-highlight
# The linearity store across processes: two remembering at once each keep
# their camera (a read-modify-write under a lock, not one's save losing the
# other's group).
mkdir -p "$smoke"/linearity
XDG_CONFIG_HOME="$smoke"/linearity "$build/ifritview" --remember-linearity camera-a 60 &
XDG_CONFIG_HOME="$smoke"/linearity "$build/ifritview" --remember-linearity camera-b 60
wait
grep -q '^\[camera-a (test)\]' "$smoke"/linearity/ifritview/linearity.conf
grep -q '^\[camera-b (test)\]' "$smoke"/linearity/ifritview/linearity.conf
! [ -e "$smoke"/linearity/ifritview/linearity.conf.lock ]
# Two arriving at an old lock file a crashed process left: neither takes it
# from the other (once, both did, the second removing the first's new lock,
# both loading and the first's group lost). Each holds the lock once it's
# read the file (IFRIT_LINEARITY_HOLD) until let go: the second doesn't read
# while the first holds, and both groups, and what was there, are kept.
store="$smoke"/linearity/ifritview
gate="$smoke"/linearity-gate
mkdir -p "$gate"
printf '[seed]\nred=0.9;\n' > "$store"/linearity.conf
touch -d '1 minute ago' "$store"/linearity.conf.lock
XDG_CONFIG_HOME="$smoke"/linearity IFRIT_LINEARITY_HOLD="$gate"/a "$build/ifritview" --remember-linearity camera-a 1 &
a=$!
XDG_CONFIG_HOME="$smoke"/linearity IFRIT_LINEARITY_HOLD="$gate"/b "$build/ifritview" --remember-linearity camera-b 1 &
b=$!
i=0
until [ -e "$gate"/a.held ] || [ -e "$gate"/b.held ]; do
  i=$((i + 1)); [ $i -lt 500 ] || { echo "neither took the linearity lock" >&2; exit 1; }
  sleep 0.01
done
if [ -e "$gate"/a.held ]; then first=a second=b; else first=b second=a; fi
sleep 1
if [ -e "$gate"/$second.held ]; then
  echo "both read the linearity store under its lock" >&2
  exit 1
fi
touch "$gate"/$first
i=0
until [ -e "$gate"/$second.held ]; do
  i=$((i + 1)); [ $i -lt 300 ] || { echo "the second never took the linearity lock" >&2; exit 1; }
  sleep 0.01
done
touch "$gate"/$second
wait $a
wait $b
grep -q '^\[seed\]' "$store"/linearity.conf
grep -q '^\[camera-a (test)\]' "$store"/linearity.conf
grep -q '^\[camera-b (test)\]' "$store"/linearity.conf
! [ -e "$store"/linearity.conf.lock ]
# One killed holding the lock lets go with it: the next goes ahead at once,
# though the lock file it left is new.
rm -f "$gate"/*
XDG_CONFIG_HOME="$smoke"/linearity IFRIT_LINEARITY_HOLD="$gate"/c "$build/ifritview" --remember-linearity camera-c 1 &
c=$!
i=0
until [ -e "$gate"/c.held ]; do
  i=$((i + 1)); [ $i -lt 500 ] || { echo "the linearity lock wasn't taken" >&2; exit 1; }
  sleep 0.01
done
kill -9 $c
wait $c || true
[ -e "$store"/linearity.conf.lock ]
XDG_CONFIG_HOME="$smoke"/linearity "$build/ifritview" --remember-linearity camera-d 1
grep -q '^\[camera-d (test)\]' "$store"/linearity.conf
! grep -q '^\[camera-c (test)\]' "$store"/linearity.conf
! [ -e "$store"/linearity.conf.lock ]
# A decode of a file that changes before the viewer takes it arrives stale;
# a file's time is kept finer than the second; and one still coming in is
# read again once it's changed (or its watch said so) and held still.
"$build/ifritview" --check-loader "$smoke"
"$build/ifritview" --check-camera-jpeg
"$build/ifritview" --check-copy-path
# Noise reduction's decode against its noise model: each end of the
# shrinkage, each colour's own law, the pure-Poisson limit, a fit of linear
# RGB, continuous in the sliders, the shrinkage's model against the
# shrinkers, and flat grey under unequal noise at strengths between.
"$build/ifritview" --check-detail
# A format plugin: the farbfeld example, built into build-ci/plugins (it isn't
# packaged), reading a 1x1 image.
printf 'farbfeld\0\0\0\1\0\0\0\1\377\377\0\0\0\0\377\377' > "$smoke"/red.ff
"$build/ifritview" --plugins
"$build/ifritview" --probe "$smoke"/red.ff
# Forgetting saved photo locations: refused while another connection still
# reads the old records (they'd stay in the journal), finished once it lets
# go and on closing; nothing written back by what was under way.
"$build/ifritview" --check-forget "$smoke"
# A raw's look and its XMP: Lightroom's profile names mapped to looks and
# written back (an unknown one kept), what counts as developed, and Save
# writing only the controls changed.
"$build/ifritview" --check-looks "$smoke"/looks
# A raw's automatic exposure: the scene's luminance from ISO, shutter and
# aperture, the display key it calls for across the mesopic range, and the
# camera's exposure kept, a stop each way at most, with and without them.
"$build/ifritview" --check-exposure
