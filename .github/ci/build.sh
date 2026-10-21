#!/bin/sh
# Configure, build, smoke-test and package. Usage: build.sh DEB|RPM
# Leaves the package and a tarball of the bare binary in dist/.
set -eu
generator=$1
arch=$(uname -m)
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr | tee build-ci.log
# A release build carries every format backend and video support.
if grep -E -- '^-- [a-z]+: (not found|disabled)' build-ci.log; then
  echo "a backend is missing; install its -dev package in .github/ci/deps-*.sh" >&2
  exit 1
fi
cmake --build build-ci --parallel "$(nproc)"

# Decode without a display.
mkdir -p build-ci/smoke
printf 'P6\n2 2\n255\n\377\0\0\0\377\0\0\0\377\377\377\377' > build-ci/smoke/rgb.ppm
build-ci/ifritview --probe build-ci/smoke/rgb.ppm

(cd build-ci && cpack -G "$generator")

rm -rf dist && mkdir -p dist/ifritview
cp build-ci/*.deb build-ci/*.rpm dist/ 2>/dev/null || true
# The bare binary, tarred so it keeps its executable bit.
strip -o dist/ifritview/ifritview build-ci/ifritview
cp README.md LICENSE dist/ifritview/
tar -C dist -czf "dist/ifritview-linux-$arch.tar.gz" ifritview
rm -r dist/ifritview
ls -l dist
