# Writable bindings and missing metadata

D3D11 rejects `CSSetUnorderedAccessViews` when two slots refer to the same writable subresource. The entire setter fails, even if a shader does not use one of the aliased slots. In `GpuView::measure`, a non-raw image bound the histogram at both u0 and u2; its statistics and peak dispatches consequently had no writable statistics buffer. Initial maximum/peak/count stayed zero, while the later volume dispatch (with unique bindings) could still measure colour extent correctly.

The statistics pass now binds only its statistics buffer at u1. The peak pass binds that buffer and clip metadata at u2. Missing metadata has a distinct zero-filled resource for its actual layout:

- clip layout: no found/censored colours or read noise; absent white scale uses peak.hlsl's existing default of one;
- levels layout: no measured black-level/noise samples;
- correction header: no gain maps, readout corrections, vignette or warp.

The raw parameters continue to say whether levels/corrections exist. Each fallback has its own resource, so PILE/DRIFT and the graphics passes can bind all statically declared resources on Vulkan without aliasing writable D3D slots. These resources are read-only inputs in the affected kernels despite their storage-buffer descriptor type. The histogram is used only for histogram data.

`--check-measures` isolates the actual measurement/render passes from the independently checked detail transport. It covers signed non-raw data, SDR, HDR, raws with and without clip metadata, and a real Bayer develop without optional levels/corrections. Both Linux smoke and Windows packaging run it. Without any headless GPU, the ordinary command returns explicit skip status77; Linux smoke reports that prerequisite skip rather than claiming a GPU pass. Other failures remain fatal. Optional image paths run real decoders and the render pipeline; the histogram requests statistics even for an SDR image that otherwise correctly skips measurement.

On Windows, `--check-measures --d3d-debug [IMAGE ...]` requires the debug layer and fails on every warning/error, including binding hazards. It fails if the layer is unavailable. Normal `--check-measures` remains usable on CI machines without Graphics Tools. The strict mode runs headlessly and never opens a window.

## Original failure proof

A standalone RTX 2080 D3D11 debug-device probe compiled the unchanged production `stats.hlsl` with FXC/O3 and dispatched the signed fixture through three binding arrangements. Unique resources and u1-only bindings both produced count/opaque 256 and maximum 0.67796129. The original duplicated u0/u2 bindings left all UAV slots null, with count/opaque/maximum zero. The debug layer reported error 2097402 (overlapping writable subresources). Both exact master 36d5784 and PR47 fc204804 artifacts independently reproduce the signed non-raw maximum-zero failure in `--check-detail`; this predates PR47.

Real-image validation must distinguish requested statistics from an intentional SDR passthrough: a plain unedited BT.709 JPEG needs no colour measurement and need not change visually. Float HDR inputs need source peak for highlight roll-off, so a zero peak can visibly change their display mapping. Record actual decoded HDR/JPEG statistics and before/after outputs, as well as the strict debug run, before declaring the fix validated.

## Combined detail/debug gate

`IFRIT_RENDER_NR=0,25 ifritview --check-measures --d3d-debug --detail IMAGE ...` runs actual decoded files with viewer opening detail defaults and the exact `IFRIT_RENDER_NR` override used by `--gpu-render`. Each file logs actual L/C/sharpness; ordinary JPEG/HDR opening defaults alone do not imply active denoising. Synthetic numeric fixtures remain detail-off. The file check uses the public detail-ready callback, waits for an actual pending completion and redraws on the wrapped debug device. Unknown checker options and `--detail` without files fail explicitly. `--d3d-debug` is not a `--gpu-render` option.

Actual-file checker renders use zoom0.25, matching the normal catalog artifact gate; source level0 and output dimensions, actual controls, fitted-noise mask, noise reads/table builds and completion wakes are logged. A fitted RAW model must be observed in the final catalog gate; legitimate unfitted JPEG/HDR cases are not mislabeled as fitted transport. The checker logs an FNV1a checksum of RGB half-floats, bottom-row-first, excluding alpha/padding. For a normal `--gpu-render IMAGE OUT.pfm ZOOM` at the same logged dimensions and controls, re-encode its PFM floats as little-endian half and compare that checksum. This checks the debug-device/callback render against the ordinary headless render without changing either production pipeline.
