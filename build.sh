#!/bin/sh
# Configure and build into ./build. Pass extra CMake arguments if needed,
# e.g. ./build.sh -DCMAKE_BUILD_TYPE=Debug
set -e
cd "$(dirname "$0")"

# On a machine without root, the GTK/format dev headers can live in a local
# sysroot unpacked from the distro's -dev packages; point pkg-config at it
# when present. Harmless when the packages are installed normally.
SYSROOT="$HOME/.local/ifrit-sysroot/root"
if [ -d "$SYSROOT" ]; then
  export PKG_CONFIG_PATH="$SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  # Some runtime libs live only in the sysroot; DT_RPATH (unlike RUNPATH)
  # also covers their own dependencies.
  set -- "$@" "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--disable-new-dtags"
fi

cmake -S . -B build -G Ninja "$@"
cmake --build build
