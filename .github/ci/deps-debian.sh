#!/bin/sh
# Build dependencies on Debian/Ubuntu (run as root inside the CI container).
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates git cmake g++ make pkg-config dpkg-dev file \
  libgtk-3-dev libexif-dev libjpeg-dev libpng-dev libwebp-dev libheif-dev \
  libjxl-dev libtiff-dev libpoppler-glib-dev libiptcdata0-dev libexempi-dev \
  libraw-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libsoup-3.0-dev libjson-glib-dev
