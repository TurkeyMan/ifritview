#!/bin/sh
# Configure, build, smoke-test and package. Usage: build.sh DEB|RPM
# Leaves the package and a tarball of the bare binary in dist/.
set -eu
generator=$1
arch=$(uname -m)
# A tag v1.2 builds version 1.2; '-' becomes '~' (a pre-release to both dpkg
# and rpm, and rpm forbids '-' in versions).
version=
if [ "${GITHUB_REF_TYPE:-}" = tag ]; then
  version=$(echo "${GITHUB_REF_NAME#v}" | tr - '~')
fi
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DIFRIT_RAPPORT_PLUGINS=ON \
  ${version:+-DIFRIT_VERSION=$version} | tee build-ci.log
# A release build carries every format backend, video support and every
# toolkit plugin.
if grep -E -- '^-- [a-z]+: (not found|disabled)' build-ci.log; then
  echo "a backend is missing; install its -dev package in .github/ci/deps-*.sh" >&2
  exit 1
fi
if ! grep -q -- '^-- rapport plugins: gtk3;gtk4;qt6$' build-ci.log; then
  echo "a toolkit plugin is missing; install its -dev package in .github/ci/deps-*.sh" >&2
  exit 1
fi
cmake --build build-ci --parallel "$(nproc)"

# Decode without a display.
mkdir -p build-ci/smoke
printf 'P6\n2 2\n255\n\377\0\0\0\377\0\0\0\377\377\377\377' > build-ci/smoke/rgb.ppm
build-ci/ifritview --probe build-ci/smoke/rgb.ppm
build-ci/ifritview --version

if [ "$generator" = DEB ]; then
  # The .deb depends on what the exe and the core need, not the plugins: a
  # desktop without a plugin's toolkit just skips it.
  mkdir -p build-ci/shlibs/debian
  printf 'Source: ifritview\n\nPackage: ifritview\nArchitecture: any\n' > build-ci/shlibs/debian/control
  depends=$(cd build-ci/shlibs && dpkg-shlibdeps -O --ignore-missing-info -l../lib/ifritview \
    ../ifritview ../lib/ifritview/librapport.so | sed -n 's/^shlibs:Depends=//p')
  echo "Depends: $depends"
  (cd build-ci && cpack -G DEB -D CPACK_DEBIAN_PACKAGE_DEPENDS="$depends")
else
  (cd build-ci && cpack -G "$generator")
fi

rm -rf dist && mkdir -p dist
cp build-ci/*.deb build-ci/*.rpm dist/ 2>/dev/null || true
# The program as installed (the exe, and the core and toolkit plugins it
# finds beside it in lib/), tarred so it keeps its executable bits.
cmake --install build-ci --prefix "$PWD/dist/ifritview" --strip
cp README.md LICENSE dist/ifritview/
suffix=${version:+-$version}
tar -C dist -czf "dist/ifritview$suffix-linux-$arch.tar.gz" ifritview
rm -r dist/ifritview
ls -l dist
