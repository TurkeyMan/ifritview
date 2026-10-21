#!/bin/sh
# Build dependencies on Fedora (run as root inside the CI container).
set -eu
dnf install -y --setopt=install_weak_deps=False \
  git cmake gcc-c++ make pkgconf-pkg-config rpm-build file \
  gtk3-devel libexif-devel libjpeg-turbo-devel libpng-devel libwebp-devel \
  libheif-devel libjxl-devel libtiff-devel poppler-glib-devel \
  libiptcdata-devel exempi-devel LibRaw-devel gstreamer1-devel \
  gstreamer1-plugins-base-devel json-glib-devel
