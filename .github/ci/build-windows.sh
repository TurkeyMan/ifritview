#!/bin/sh
# Cross-build for Windows, then stage it as an install tree (bin\ with the exe
# and every DLL it reaches, lib\ with gdk-pixbuf's loaders and GIO's modules),
# zipped and as an NSIS installer. Leaves both in dist/.
set -eu
mingw=/usr/x86_64-w64-mingw32/sys-root/mingw
version=
if [ "${GITHUB_REF_TYPE:-}" = tag ]; then
  version=$(echo "${GITHUB_REF_NAME#v}" | tr - '~')
fi
mingw64-cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ${version:+-DIFRIT_VERSION=$version}
cmake --build build-win --target ifritview --parallel "$(nproc)"

stage=dist/IfritView
rm -rf dist && mkdir -p "$stage/bin" "$stage/lib"
x86_64-w64-mingw32-strip -o "$stage/bin/ifritview.exe" build-win/ifritview.exe
# gdk-pixbuf's loaders (GIF and ICO are built in; these are the minor formats)
# and GIO's TLS module (geocoding over HTTPS), where Windows looks for them:
# relative to the DLLs' directory.
cp -r "$mingw/lib/gdk-pixbuf-2.0" "$stage/lib/"
mkdir -p "$stage/lib/gio" && cp -r "$mingw/lib/gio/modules" "$stage/lib/gio/"
find "$stage/lib" -name '*.a' -delete
python3 .github/ci/windows-deploy.py "$stage" "$mingw/bin"
cp README.md LICENSE "$stage/"

app_version=${version:-$(sed -n 's/^project(IfritView VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}
suffix=${version:+-$version}
(cd dist && zip -qr "ifritview$suffix-windows-x86_64.zip" IfritView)
makensis -V2 -DVERSION="$app_version" -DSTAGE="$PWD/$stage" \
  -DOUT="$PWD/dist/ifritview$suffix-windows-x86_64-setup.exe" "$PWD/data/windows/ifritview.nsi"
rm -r "$stage"
ls -l dist
