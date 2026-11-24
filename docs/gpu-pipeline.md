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

Noise fitting stays on the GPU. Its compact model and expectation-inverse
tables are read back once per source. CPU workers call the same
`noise_shaping` and `noise_decode_table` functions as the CPU develop, then
the render thread uploads the 1024-entry RGB shift table. Image pixels do
not come back for this work. The per-pixel shader only filters and looks up
the result; it does not contain the per-image matrix solves.

The shared CPU decode evaluates the table grid and own-inverse operand in
double before taking their small residual. Stored inverse ordinates and the
covariance/shrinker model remain float; the final shift is rounded once.
This preserves the tabulated model without amplifying avoidable float
rounding when the retained noise share is large. The checks compare these
operations with a long-double control, including quiet and near-identity
cases.

The cache pins the noise buffer's identity and includes the source
generation, noise-write revision and every Detail setting. Retained sources
keep their snapshots and completed tables. Superseded or discarded sources
cancel their queued work; only the current request can upload a result.
A noise snapshot becomes usable only when its readback callback records
delivery. Discarding the last uncalled callback records failure, including
an already-done default ticket; legitimate delivered zero data stays valid.
Failure clears the request cache so the next develop retries. Worker waits
are cancellation-aware, and shutdown joins without requiring pending GPU
callbacks or UI pumping.
The viewer keeps its last frame and schedules a repaint when its worker is
ready, without waiting on that worker. Headless rendering and synchronous
pixel exports wait for the same result. `--check-detail` exercises the
upload, quiet-axis reference and asynchronous cache transitions.

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
  Set `IFRIT_RENDER_NR=0,25` to exercise the viewer's opening RAW detail
  defaults too, including sharpening; an ordinary headless render leaves
  detail controls at rest.
- `ifritview --check-brightness` runs the brightness curve's shaping code
  (`src/shaders/brightness_curve.hlsli`, `brightness_shape.hlsli`) on the
  CPU over made-up histograms and checks its slopes, knee and top.
- `ifritview --process IN [exposure:EV] gpu diff:REFERENCE` runs the
  separate headless sample renderer in `src/gpu.cpp`. Use a CPU-rendered
  reference for the same input and exposure when comparing the conversion
  of source samples to eight-bit pixels.
- For native presentation, exercise the viewer on the affected backend.
  A headless render tests the image before the toolkit presents it.

Follow-up: overlap CPU table construction with remaining develop work where
its input dependencies permit, and add a warm-develop no-regression gate.
The current measured once-per-input trade-off is 2–7% higher whole-process
warm wall time on several controls, alongside substantially faster detail
filter stages and removal of the large first-draw stall.
