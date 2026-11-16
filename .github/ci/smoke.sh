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
# The viewer's downscaler, which draws every zoomed-out image (GCC 16.2's
# LTO once miscompiled it): a 64x48 image to 23x17.
printf 'P6\n64 48\n255\n' > "$smoke"/big.ppm
head -c $((64 * 48 * 3)) /dev/urandom >> "$smoke"/big.ppm
"$build/ifritview" --process "$smoke"/big.ppm shrink:23,17 info
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
# A format plugin: the farbfeld example, built into build-ci/plugins (it isn't
# packaged), reading a 1x1 image.
printf 'farbfeld\0\0\0\1\0\0\0\1\377\377\0\0\0\0\377\377' > "$smoke"/red.ff
"$build/ifritview" --plugins
"$build/ifritview" --probe "$smoke"/red.ff
# Forgetting saved photo locations: refused while another connection still
# reads the old records (they'd stay in the journal), finished once it lets
# go and on closing; nothing written back by what was under way.
"$build/ifritview" --check-forget "$smoke"
