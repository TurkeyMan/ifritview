# IfritView

A light, no-frills, HiDPI-aware image viewer for Linux in the spirit of
IrfanView. GTK3, C++20, and the system's own format libraries.

## Behaviour

- The window resizes to show each image at 1:1, but never smaller than a
  configurable minimum (640×480 by default) and never larger than the work
  area; images that don't fit are scaled down to fit.
- The toolbar, footer and dialogs follow the desktop scale factor. The image
  panel does not: 100% is always one image pixel per screen pixel.
- Double-click or Enter toggles fullscreen; Esc leaves fullscreen (or quits).
- Mouse wheel steps through the folder, Ctrl+wheel zooms around the cursor,
  drag pans.
- Images decode on a worker thread; flicking through a folder only decodes
  what ends up on screen.

| Key | Action |
|---|---|
| Left / PageUp / Backspace | previous image |
| Right / PageDown / Space | next image |
| Home / End | first / last image |
| Enter / F11 / double-click | toggle fullscreen |
| Esc | leave fullscreen, else quit |
| + / - / Ctrl+wheel | zoom in / out |
| 1 | actual pixels |
| F | fit to window |
| R / F5 | rescan folder and reload |
| Ctrl+O | open file |
| Ctrl+P | settings |
| Ctrl+Q | quit |

Settings live in `~/.config/ifritview/ifritview.conf` (a GKeyFile), including
the list of extensions considered when cycling through a folder.

## Formats

| Format | Library |
|---|---|
| JPEG (incl. CMYK, EXIF orientation) | libjpeg-turbo, libexif |
| PNG | libpng |
| WebP (incl. animated, first frame) | libwebp |
| HEIF / HEIC / AVIF | libheif |
| JPEG XL | libjxl |
| TIFF | libtiff |
| Camera RAW: Canon CR2/CR3/CRW, Nikon NEF, Fuji RAF, Sony ARW, DNG, Olympus, Panasonic, Pentax, … | LibRaw |
| PSD (flattened), TGA, PNM, HDR, PIC | vendored stb_image |
| BMP, GIF, ICO, XPM, SVG and anything else with a loader | gdk-pixbuf |

Each backend is optional: a missing library is reported at configure time and
that decoder is left out.

## Building

    sudo apt install cmake ninja-build libgtk-3-dev libjpeg-dev libexif-dev \
        libpng-dev libwebp-dev libheif-dev libjxl-dev libtiff-dev libraw-dev
    ./build.sh
    ./build/ifritview some/picture.jpg

`build.sh` also knows about a local sysroot at `~/.local/ifrit-sysroot` for
machines where the `-dev` packages can't be installed system-wide.

## Development aids

- `ifritview --probe files...` decodes files and prints what each backend
  made of them, without opening a window.
- `IFRIT_SCRIPT="next;fullscreen;shot:out.png;quit" ifritview dir/` drives the
  UI from a list of actions and renders windows to PNG, so behaviour can be
  checked headlessly (and on Wayland, where screen capture is otherwise
  unavailable to the app).
- `cmake --build build --target mkimages && ./build/mkimages out/` plus
  `tools/mkimages.py out/` generate a folder of test images in every format
  that can be encoded with the linked libraries.
