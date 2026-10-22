# IfritView

IrfanView clone, but portable, and kinda better!

IfritView is a fast, light image viewer in the spirit of IrfanView: open a
picture, flick through its folder, and get on with your day. It runs natively
on Linux and Windows, and opens almost anything a camera, a phone, the web or
an old game ever produced.

## Principles

- **Speed is the feature.** The image you asked for comes before everything
  else. Prefetching, raw development and metadata happen around it, never in
  its way, and flicking through a folder shows every image, not just the
  last one.
- **Pixels are honest.** The window fits the image at 1:1 whenever it can,
  and 100% always means one image pixel per screen pixel, whatever the
  desktop's scale. Anything that changes how an image looks, such as its
  orientation or an edit recorded in its metadata, is applied before it is
  first shown.
- **Native everywhere.** The interface is built from each platform's own
  controls through [rapport](https://github.com/TurkeyMan/rapport), and
  images are decoded by the platform's own format libraries.
- **Nothing is required that isn't needed.** Every format library is
  optional. Build without one and its formats are simply left out.
- **Your photos stay yours.** The only thing IfritView sends anywhere is a
  photo's GPS position, to turn it into a place name, and you choose the
  service or switch it off in Settings.

## What it does

- **Opens nearly everything:** camera raw from every major maker, phone
  photos and modern formats, the web's formats, layered and print formats,
  phone and camera video, and the formats of old consoles and home computers.
  Raw files show their embedded preview at once and are developed in the
  background.
- **Shows what's inside:** EXIF, maker notes, IPTC and XMP (sidecars
  included), with GPS positions as map links and place names.
- **Gets out of the way:** fullscreen with an optional info overlay,
  slideshows, zoom and pan by mouse or keyboard, and mouse controls you can
  rebind for windowed and fullscreen use.
- **Saves to other formats,** with each format's own settings and a live
  preview of lossy encoding before anything is written.

## Building

    git clone --recursive https://github.com/TurkeyMan/ifritview
    cd ifritview
    ./build.sh

Install your platform's development packages for the toolkit and the format
libraries you want first. The configure step reports what it found and what it
left out.

## Development

IfritView can be driven without a person at the keyboard. `--probe` reports how
each file decodes, `--convert` re-encodes from the command line, `IFRIT_SCRIPT`
runs a scripted session, and `IFRIT_TRACE` times every step from keypress to
pixels. Where it's heading is in [docs/roadmap.md](docs/roadmap.md).

## Licence

GPL-3.0. See [LICENSE](LICENSE).
