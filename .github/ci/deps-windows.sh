#!/bin/sh
# Fedora's MinGW cross toolchain and the Windows builds of the format
# libraries, plus NSIS for the installer. Run as root in a fedora container.
# No PDF yet: poppler is loaded at runtime, so it would need bundling by hand.
set -eu
dnf install -y \
  git cmake ninja-build pkgconf zip python3 mingw32-nsis \
  mingw64-gcc-c++ mingw64-binutils mingw64-winpthreads-static \
  mingw64-glib2 mingw64-glib-networking mingw64-gdk-pixbuf mingw64-json-glib \
  mingw64-libjpeg-turbo mingw64-libpng mingw64-libwebp mingw64-libtiff \
  mingw64-LibRaw mingw64-lcms2
