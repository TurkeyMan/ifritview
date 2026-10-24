#!/usr/bin/env python3
"""The licence notices of the code IfritView carries, for shipping with it.

  third-party-notices.py bundled OUT
      The code in this repository from elsewhere (third_party/): checked in as
      third_party/NOTICES.txt, which Linux packages install, their libraries
      being the distribution's own.

  third-party-notices.py windows VCPKG_SHARE OUT
      That, and every vcpkg library the Windows build links statically, from
      each port's copyright file (VCPKG_SHARE is <installed>/<triplet>/share):
      THIRD-PARTY-NOTICES.txt beside ifritview.exe.

The ports are listed below, as the link line has them; a port whose copyright
file is missing stops the script rather than ship without it.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# vcpkg ports linked into ifritview.exe: (port, what it is).
PORTS = [
    ("aom", "AOM AV1 codec (AVIF)"),
    ("gdk-pixbuf", "GdkPixbuf"),
    ("gettext-libintl", "GNU libintl"),
    ("glib", "GLib, GIO, GObject"),
    ("json-glib", "JSON-GLib"),
    ("lcms", "Little CMS"),
    ("libde265", "libde265 (HEIC)"),
    ("libexif", "libexif"),
    ("libffi", "libffi"),
    ("libheif", "libheif (HEIC, AVIF)"),
    ("libiconv", "GNU libiconv"),
    ("libjpeg-turbo", "libjpeg-turbo"),
    ("liblzma", "XZ Utils liblzma"),
    ("libpng", "libpng"),
    ("libraw", "LibRaw (camera raw files)"),
    ("libspng", "libspng"),
    ("libwebp", "libwebp"),
    ("pcre2", "PCRE2"),
    ("tiff", "libtiff"),
    ("x265", "x265 (HEIC encoding)"),
    ("zlib", "zlib"),
]

# Ports whose vcpkg copyright file only points elsewhere: the licence itself.
OVERRIDES = {"pcre2": ROOT / "third_party/licenses/pcre2-LICENCE.md"}


def header_licence(path, start):
    """The licence block at the end of a single-header library."""
    text = path.read_text(encoding="utf-8", errors="replace")
    i = text.rfind(start)
    if i < 0:
        sys.exit(f"{path}: no licence block ({start!r})")
    return text[i:].strip().removeprefix("/*").removesuffix("*/").strip()


def bundled():
    third = ROOT / "third_party"
    return [
        ("rapport (UI toolkit)", (third / "rapport/LICENSE").read_text()),
        ("ASTC decoder, from Basis Universal (Apache License 2.0)",
         (third / "astc/NOTICE").read_text() + "\n\n" + (third / "astc/LICENSE").read_text()),
        ("stb_image, by Sean Barrett", header_licence(third / "stb_image.h", "This software is available under 2 licenses")),
        ("bcdec, by Sergii Kudlai", header_licence(third / "bcdec/bcdec.h", "This software is available under 2 licenses")),
        ("etcdec, by Sergii Kudlai", header_licence(third / "etcdec/etcdec.h", "This software is available under 2 licenses")),
        ("bc7enc and rgbcx, by Richard Geldreich, Jr. (DDS saving)", (third / "bc7enc/LICENSE").read_text()),
        ("Octicons, by GitHub (the GitHub mark in About)", (third / "licenses/octicons-LICENSE").read_text()),
    ]


def ports(share):
    out = []
    for port, what in PORTS:
        f = OVERRIDES.get(port, share / port / "copyright")
        if not f.is_file():
            sys.exit(f"no licence for {port}: {f}")
        out.append((f"{what} [{port}]", f.read_text(encoding="utf-8", errors="replace")))
    return out


def write(out, intro, sections):
    rule = "=" * 78
    parts = [intro.strip(), ""]
    parts += [f"  - {name}" for name, _ in sections]
    for name, text in sections:
        text = re.sub(r"[ \t]+\n", "\n", text.replace("\r\n", "\n")).strip()
        parts += ["", rule, name, rule, "", text]
    pathlib.Path(out).write_text("\n".join(parts) + "\n", encoding="utf-8", newline="\n")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "bundled":
        write(sys.argv[2], """
IfritView includes code from these projects, under the licences that follow.
IfritView itself is GPL-3.0-or-later (see LICENSE).""", bundled())
    elif len(sys.argv) == 4 and sys.argv[1] == "windows":
        write(sys.argv[3], """
IfritView for Windows includes code from these projects, built into
ifritview.exe, under the licences that follow. IfritView itself is
GPL-3.0-or-later (see LICENSE).""", bundled() + ports(pathlib.Path(sys.argv[2])))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
