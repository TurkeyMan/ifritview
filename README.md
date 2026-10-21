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
| L / R | rotate left / right |
| H / V | mirror horizontally / vertically |
| F5 | rescan folder and reload |
| Ctrl+O | open file |
| Ctrl+P | settings |
| Ctrl+Q | quit |

Settings live in `~/.config/ifritview/ifritview.conf` (a GKeyFile), including
the list of extensions considered when cycling through a folder.

In fullscreen a small info overlay can show categories of metadata (file,
format, shot, location, view); Settings → Viewing picks the categories,
colour and size, and whether it also shows in the window.

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
| PDF (first page) | poppler-glib |
| BMP, GIF, ICO, XPM, SVG and anything else with a loader | gdk-pixbuf |

Metadata shown in the Metadata window (`I`): EXIF/GPS/maker notes (libexif),
IPTC-IIM (libiptcdata), XMP embedded or from a `.xmp` sidecar (exempi),
PNG text chunks, TIFF tags, LibRaw camera data and PDF document info.

Each backend is optional: a missing library is reported at configure time and
that decoder is left out.

## Building

    sudo apt install cmake ninja-build libgtk-3-dev libjpeg-dev libexif-dev \
        libpng-dev libwebp-dev libheif-dev libjxl-dev libtiff-dev libraw-dev \
        libpoppler-glib-dev libiptcdata0-dev libexempi-dev
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

## Roadmap

- Decoder backends as plugin `.so`s loaded with `g_module_open`, so a package
  can *Recommend* the format libraries instead of requiring them. (Startup is
  not the motivation: the dynamic loader is ~4 ms of the ~140 ms warm start;
  the rest is GTK's own window setup.)
- **Raw develop pipeline** (the big one; needed before light editing + save
  is worth anything). Today: LibRaw to scene-linear sRGB, a tone curve
  histogram-matched to the embedded JPEG, exposure as a gain. The camera JPEG
  still differs in colour (white balance rendering, film simulation / picture
  style), tone, and geometry (in-camera lens corrections). Plan:
  - take camera-space linear from LibRaw (no matrix, unit multipliers) and own
    white balance + colour matrix + tone in our pass; WB slider with D50/D65
    ticks and an as-shot estimate;
  - lens corrections from embedded parameters (Fuji/Sony/Panasonic maker
    notes, DNG WarpRectilinear) with lensfun as the fallback (Canon);
  - a tone/colour model that gets closer to the camera look (per-make picture
    styles are a long tail; start with a filmic base and the matched curve);
  - threaded CPU demosaic first, then the whole thing on the GPU (demosaic,
    WB, matrix, warp, tone in shaders) shared with the HDR presenter.
  - edits (exposure, WB, crop, rotation) kept as parameters and saved to an
    XMP sidecar; export applies them to the developed image.
- Honour `crs:CropAngle`, `crs:Tint`.
- Hand-written decoders for XCF, TIM, IFF/ILBM; animated GIF/WebP playback.
- Re-render toolbar icons when the icon theme changes at runtime (currently
  cached per theme/scale/colour and picked up on the next start).
