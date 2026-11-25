# Camera JPEG and the viewport

Turning the develop panel's Camera JPEG on or off shows another image of the
same raw. It is a different size and may frame a different part of the
sensor, but it is the same scene, so the view stays where it was.

## Registering the two images

`RawMosaic::camera_crop` is the camera JPEG's framing on the sensor (LibRaw's
inset crop, in active-area pixels), with `camera_orientation` as the camera
held it. It is kept whether or not the develop crops to it, which is what
`RawMosaic::crop` says. A develop is registered on the sensor by `crop` and
`orientation` (which include the user's turns and crops). The camera's JPEG,
at whatever size it was decoded (the stand-in fitted to the view, or whole),
is registered by `camera_crop`, `camera_orientation` and its own
`Image::transform`.

`View::replace_image(image, raw)` (the toggle, either way) and
`View::image_developed(before)` (a develop landing in place of the camera's
JPEG it showed) keep the point of the scene at the view's centre, in sensor
pixels, and keep the screen pixels each sensor pixel takes. Fit stays Fit:
each is then shown whole, even where they frame the sensor differently.
Zoom limits and pan bounds still apply.

The model: the camera's JPEG is its inset of the sensor. That is claimed
where the raw gives an inset wholly inside the decoded active area and the
JPEG's aspect matches it to a pixel, whatever the make or format. Anything
else (no inset, an inset past the active area as on the R5, a thumbnail of
another aspect, LibRaw's develop, a CPU develop with no mosaic) keeps the
same fraction across at the same shown size, as a develop replacing its
stand-in always has. A JPEG the camera corrected for its lens's distortion
is not a crop of the sensor; nothing in the raw reliably says it was, so
that residual is accepted (below).

## Describing what is shown

`App::shown_image()` is the view's image: what is on screen, even while
another is being read to replace it. `App::shown_format()` names
it: "Camera JPEG", "LibRaw develop" or its format. The status bar, the info
panel's Image group and the fullscreen HUD describe that image (size, format,
depth, pixel format, colour space, zoom against its own 1:1). The file's size
and the file's metadata groups (Camera, Raw, EXIF, ...) stay the file's.

A raw's preview carries the develop's size (`final_width`), so it stands in
at the develop's scale. The camera's JPEG shown as itself stands in for
nothing: that size is dropped, and its own pixels are its 1:1 for Fit, the
zoom shown and the window.

## Registration evidence

CPU-only LibRaw camera-WB cropped develop against the actual embedded JPEG:
log-luminance gradients at 1024 px width, normalized cross correlation
globally and on nine local patches. Measured offsets are never applied as
corrections.

| Sample | Active-area inset | Global residual x,y | Largest local residual |
| --- | --- | --- | --- |
| Canon 5D III 5G4A9394.CR2 | 18,16,5760,3840 | -0.001,+0.003 | 0.34 |
| Canon 5D III M-RAW _T0B7734.CR2 (portrait; JPEG 5760x3840) | 0,0,3960,2640 | +0.005,-0.037 | 0.07 |
| Canon 6D EOS_6D_mRAW.CR2 | 0,0,4104,2736 | +0.055,-0.027 | 0.36 |
| Canon 1D X, 350D, 5D II, 5D IV, 7D (CR2) | camera's | within 0.41 | within 0.73 |
| Canon 90D cRAW, R6 (CR3) | 12,12,... | within 0.07 | within 0.38 |
| Nikon D850 12- and 14-bit NEF | 16,8,8256,5504 | +0.064,+0.010 | 0.21 |
| Pentax K-1, K10D; Samsung GX20 (DNG) | camera's | within 0.08 | within 0.62 |

Cameras that correct their lens's distortion in the JPEG (Canon M50 with its
kit lens, PowerShot G7 X; Leica M9, M10-R, Q, SL; Pixel phones; Xiaomi;
Hasselblad L1D) differ from any crop of the sensor by that correction: a
radial residual of some 5 to 35 raw px towards the frame's edges, a few px
near its centre. Their framing is still the best registration there is; no
framing can express the rest.

The R5 samples report an inset (8192x5464 for the no-crop ones, as the JPEG)
one pixel larger each way than the decoded active area (8191x5463). It
extends outside it, so it is not registered; no clamp, added pixel or
model-specific offset is inferred. Its JPEG is within 2.5 raw px of the
same fraction across.

tools/check-camera-framing.cpp reproduces the measurement without a GPU or
display:

    g++ -O2 -std=c++20 tools/check-camera-framing.cpp \
        $(pkg-config --cflags --libs libraw_r libjpeg) -o check-camera-framing
    OMP_NUM_THREADS=2 ./check-camera-framing example.CR2 example.CR3 example.NEF example.DNG

`ifritview --check-camera-jpeg` exercises the toggle through the app on a
non-painting 800x600 canvas: registered and unknown framing both ways, Fit,
all eight camera orientations with the same and differing framings, the
user's crop and turn of the JPEG, the JPEG's own 1:1, a develop landing
after switching off mid-develop, the next raw with the toggle on, and the status bar and info
panel describing what is shown.
