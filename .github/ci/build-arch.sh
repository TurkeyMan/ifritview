#!/bin/sh
# Build, check, lint and install-test the Arch package from
# packaging/arch/PKGBUILD (as root in the CI container; makepkg itself runs
# as an unprivileged user). Leaves the package in dist/.
set -eu
src=$PWD
# A tag v1.2 builds 1.2; pkgver can't hold '-', and 1.3beta sorts before 1.3.
version=
if [ "${GITHUB_REF_TYPE:-}" = tag ]; then
  version=$(echo "${GITHUB_REF_NAME#v}" | tr -d -)
fi

id builder >/dev/null 2>&1 || useradd -m builder
# The checkout is root's; the builder reads it (pkgver asks git about it).
git config --system --add safe.directory "$src"
work=$(mktemp -d)
cp packaging/arch/PKGBUILD "$work/"
chown -R builder "$work"
(cd "$work" && runuser -u builder -- env IFRIT_SOURCE="$src" ${version:+IFRIT_VERSION=$version} \
  BUILDDIR="$work/build" PKGDEST="$work/out" makepkg -f --noconfirm)
pkg=$(ls "$work"/out/ifritview-[0-9]*.pkg.tar.zst)

# Lint: errors fail the build, warnings are only shown.
namcap "$pkg" | tee "$work/namcap.log"
if grep -q ' E: ' "$work/namcap.log"; then
  echo "namcap found errors" >&2
  exit 1
fi

# Installed, it runs from /usr and finds its private core in /usr/lib/ifritview.
pacman -U --noconfirm "$pkg"
mkdir -p "$work/smoke"
printf 'P6\n2 2\n255\n\377\0\0\0\377\0\0\0\377\377\377\377' > "$work/smoke/rgb.ppm"
ifritview --version
ifritview --probe "$work/smoke/rgb.ppm"

rm -rf dist && mkdir -p dist
cp "$pkg" dist/
ls -l dist
