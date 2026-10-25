# Roadmap

- **GPU image pipeline** (in progress; see [gpu-pipeline.md](gpu-pipeline.md)):
  images on the GPU in their native format, viewing and tone mapping in
  shaders, presentation without leaving the GPU, then HDR output. The
  raw develop pipeline below is built on it.
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
- Hand-written decoder for XCF.
- Re-render toolbar icons when the icon theme changes at runtime (currently
  cached per theme/scale/colour and picked up on the next start).
