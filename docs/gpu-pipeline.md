# The GPU image pipeline

The groundwork for the raw develop pipeline and the HDR pass, which need the
same thing: the image on the GPU in the form the file has it, and everything
between that and the screen done there in floating point.

## Shape

1. **Upload as decoded.** An image goes to the GPU once, in its own sample
   format: 8-bit (sRGB-encoded), 16-bit, half or float (linear), a raw's
   linear camera data, later a raw's mosaic. Nothing is converted to 8-bit
   on the way.
2. **Process in passes.** A chain of shader passes into half-float targets.
   For viewing: linearise, exposure, tone map (for HDR material, driven by
   the exposure slider), output transform (sRGB today; the display's colour
   space and HDR range later). Developing inserts its passes at the front:
   demosaic, white balance, colour matrix, lens-distortion correction.
   Edits are parameters of passes, saved to XMP; nothing is baked until
   export. Three stages: the develop (source-specific: decode, demosaic,
   corrections) gives scene-linear data; the edit (view.hlsli's `edit()`,
   the same for every source: white balance and matrix, exposure, the
   default look, Lightroom's controls as adjust.h's tables) is what export
   saves; the display mapping (tone and gamut to the display, encoding) is
   the view's alone.
3. **Present without leaving the GPU.** The result goes to the screen as a
   texture the toolkit composites, with the app's overlays (text, the
   selection, the HUD) drawn over it as today. HDR presentation hands the
   toolkit a high bit-depth buffer with its colour space: DXGI scRGB/HDR10
   on Windows, a colour state on GTK 4, Qt's HDR swapchain.

Without a usable GPU (no driver, a remote session) the same passes run on
Mesa's software renderer, and failing that the current CPU path stands.

## APIs: OpenGL ES 3 on Linux, Direct3D 11 on Windows

Each platform's native GPU API, every pass written once for each (GLSL and
HLSL), behind one small interface (upload, run a pass, export a frame):

- **Linux:** OpenGL ES 3.0 through EGL; surfaceless contexts for work
  without a window (`--process`, saving, thumbnails), which also run under
  llvmpipe. Frames leave as dmabufs (`EGL_MESA_image_dma_buf_export`).
- **Windows:** Direct3D 11, rendering into a shared texture on the adapter
  the canvas presents on; the DXGI swapchain it ends in is the only route to
  HDR on Windows, and D3D11 is there on every machine (Remote Desktop, VMs,
  Windows on ARM), where OpenGL often isn't.

Vulkan could later replace OpenGL on Linux (explicit dmabuf export and sync,
compute) behind the same interface, if GL ever limits us.

## Contexts and threads

The processing context belongs to IfritView, not the toolkit: made on the
loader's side, current on one worker thread, rendering offscreen. A finished
frame crosses to the presenting toolkit without a copy where the platform
allows.

rapport stays free of either API: it presents a frame the app hands it (a dmabuf on
Linux, a D3D11 shared texture on Windows) as the canvas's image layer, and
`on_paint` keeps drawing the overlay above it. `present()` keeps only the
newest frame and asks for a redraw; the toolkit's frame clock still paces,
and a release callback on the UI thread says when rapport has finished
reading a frame. rapport also says which GPU it presents on (a DXGI adapter
LUID; a DRM render node) and which formats and modifiers it can import, so
the app makes its context on that GPU and allocates frames the toolkit can
take without a copy.

| Backend | Presentation of a GPU frame | HDR |
|---|---|---|
| Win32 | a flip-model DXGI swapchain on the canvas's window: the app's shared texture copied into the back buffer, the overlay drawn over it with Direct2D, then presented | scRGB (FP16), overlay colours scaled to the SDR white level |
| GTK 4 | a `GdkDmabufTexture` in the canvas's snapshot, under the overlay | colour states (GTK ≥ 4.16), shown as HDR where the compositor has Wayland's colour-management protocol |
| Qt 6 | a `QOpenGLWidget` importing the dmabuf (`EGL_EXT_image_dma_buf_import`) | none from widgets: tone-mapped SDR |
| GTK 3 | read back into the cairo surface | none (8-bit) |

Order: the CPU-pixels path everywhere first (so the app works on every
backend from the start), then Win32 and GTK 4, then Qt; GTK 3 keeps the
readback. HDR comes first on Windows, then GTK 4 where the compositor
supports it; elsewhere HDR material is tone-mapped to SDR.

Reading the frame back to the CPU each time and drawing it into the toolkit's
own buffer is kept only as the fallback: it costs a full copy per frame and
an 8-bit buffer rules out HDR.

## Steps

1. **GPU core in IfritView** (no window): context, upload in native formats,
   the viewing passes, readback. Checked against today's CPU rendering with
   `--process FILE [exposure:EV] gpu diff:cpu.png`, so it starts as a
   drop-in. *Done (src/gpu.cpp on rapport's rap::gpu, shaders/render.hlsl):
   16-bit, half and float sources and a raw's linear data with its tone
   curve render identically to the CPU on Vulkan (lavapipe) and Direct3D 11,
   or within a level or two on a few pixels (float against double).*
2. **GPU canvas in rapport**: a canvas that presents a GPU frame under the
   painter's overlay, SDR, in all four backends with the fallback.
3. **IfritView on the GPU canvas**: zoom, pan, mip filtering and exposure
   become uniforms rather than re-renders.
4. **HDR presentation**: high bit-depth targets and colour spaces per backend;
   the deferred HDR pass (PQ/HLG photos, JPEG XL float) lands here.
5. **Developing**: camera-space data from LibRaw, demosaic, white balance,
   colour matrix, lens corrections, edits as XMP.
