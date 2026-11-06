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
# A format plugin: the farbfeld example, built into build-ci/plugins (it isn't
# packaged), reading a 1x1 image.
printf 'farbfeld\0\0\0\1\0\0\0\1\377\377\0\0\0\0\377\377' > "$smoke"/red.ff
"$build/ifritview" --plugins
"$build/ifritview" --probe "$smoke"/red.ff
