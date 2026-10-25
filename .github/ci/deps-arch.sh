#!/bin/sh
# Build dependencies on Arch (run as root inside the CI container): the
# PKGBUILD's depends and makedepends, and namcap to lint the package.
set -eu
pacman -Syu --noconfirm --needed \
  base-devel git cmake namcap \
  cairo exempi gdk-pixbuf2 glib2 gtk3 gtk4 wayland-protocols qt6-base json-glib lcms2 libexif libheif libiptcdata \
  libjpeg-turbo libjxl libpng libraw libtiff libwebp zlib zstd bzip2 \
  poppler-glib gstreamer gst-plugins-base-libs
