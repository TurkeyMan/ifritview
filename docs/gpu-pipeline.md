# The GPU image pipeline

IfritView processes images on Rapport's GPU device. IfritView owns the
image formats, shader passes and editing decisions. Rapport provides the
device, textures, command submission and presentation to the native canvas.
The shaders in `src/shaders` are HLSL. The build compiles them for Vulkan
on Linux and Direct3D 11 on Windows.

## From the file to the screen

1. **Prepare the source.** The loader decodes the file. Depending on the
   format, the image holds display pixels, higher-depth samples, linear RAW
   data or a sensor mosaic. Some RAW formats retain their compressed
   readout so the GPU can unpack it. A RAW preview can be shown while the
   full image is being prepared.
2. **Develop and edit.** `GpuView` uploads the source on the canvas's
   device and retains textures for reuse. RAW passes unpack, correct and
   demosaic as required. The view shaders apply exposure and the develop
   controls. The resampling passes draw the portion of the image visible
   at the current zoom.
3. **Present.** The view selects an output format for the canvas and maps
   to it. `GpuView::begin_frame` allocates the render target and describes
   its colour encoding. The view hands the texture, completion ticket and
   colour description to `canvas::present`. The canvas also draws the
   controls and overlays. CPU consumers such as the clipboard can request
   pixels separately through the readback path.

The application side of this boundary is in `src/ui/view.cpp` and
`src/ui/gpu_view.cpp`. The Vulkan and Direct3D implementations are in the
pinned Rapport tree under `core/src/gpu_vulkan.cpp` and
`core/src/gpu_d3d11.cpp`.

## Checking a rendering path

- `ifritview --gpu-render IN OUT.ppm ZOOM [FILTER]` creates a headless
  Rapport device and exercises the window's GPU renderer. Use it to
  investigate the view's resampling and composition without a window.
  An OUT ending `.pfm` is written as linear float (1 = SDR white), and
  `IFRIT_DISPLAY_PEAK=4` renders for a display four times SDR white, so
  the HDR mapping can be checked headless.
  `IFRIT_REDRAW=wb:R,G,B` (or `detail`) draws it a second time with that
  white balance (or the raw defaults' noise reduction and sharpening), and
  `IFRIT_CURVE_LOG=1` prints each brightness curve made, so it can be seen
  to follow the edit.
- `ifritview --check-brightness` runs the brightness curve's shaping code
  (`src/shaders/brightness_curve.hlsli`, `brightness_shape.hlsli`) on the
  CPU over made-up histograms and checks its slopes, knee and top.
- `ifritview --process IN [exposure:EV] gpu diff:REFERENCE` runs the
  separate headless sample renderer in `src/gpu.cpp`. Use a CPU-rendered
  reference for the same input and exposure when comparing the conversion
  of source samples to eight-bit pixels.
- For native presentation, exercise the viewer on the affected backend.
  A headless render tests the image before the toolkit presents it.
