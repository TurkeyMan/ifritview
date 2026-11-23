# IfritView design review

7 November 2026 · IfritView `068944975cdb0a21a9b0fbe0520aebb6c490993f` · Rapport `3269ddc890fe1b3d790413e7233f72488fd8e56e`

The largest design debt is concentrated at three boundaries. A decoded `Image` now also serves as the editable document, display cache, and input to an asynchronous export. The loader has taken ownership of file I/O without taking ownership of the decision about which files should be read into memory. Metadata export still has its own XML implementation after sidecar editing acquired a more capable one. Each boundary already produces observable differences between ordinary user actions.

This is a backlog for maintainers. IDs stay fixed as items are addressed. **High** means the consequence can include disclosure, damaged output, a race, or resource use large enough to prevent opening a file. **Medium** covers incorrect behaviour with a narrower impact or a structural change worth scheduling. **Low** covers inexpensive consolidation. “Reproduced” means a focused executable exercised the production function. “Traced” means the call path was verified in source. Acceptance checks describe future verification.

## Findings

### D01 · High · Two XML implementations disagree about which XMP to strip

**Reproduced.** With `MetadataChoice.location = false`, `edit_xmp` retained the latitude in `<GPSLatitude xmlns="http://ns.adobe.com/exif/1.0/">12,34N</GPSLatitude>` inside an RDF description. In the same harness, the parser used for sidecars correctly returned the EXIF namespace and local name. The export parser records only prefixed declarations and requires a colon in the property name. An unprefixed element with a default namespace is valid XML. Export consequently leaves the coordinate in a file the user asked to strip.

The project already has a namespace-aware document with source spans. Use it for the export property walk, group detection, removal, and fixed-value edits. Keep the export policy in the metadata module. Sidecar preservation, redaction and the policy for malformed packets need separate tests even when they share a parser. The separate writer also copies raw attribute contents into double quotes, which can invalidate an originally single-quoted attribute containing a literal double quote.

**Acceptance.** Read, list, strip and re-read the same packets with prefixed and default namespaces, prefix rebinding, unknown properties, quoted attribute values, and malformed input. Inspect bytes in the exported file for the removed location. Sources: `src/metadata/edit.cpp` lines 337, 464, 545, 592, 744; `src/decoders/xmp.cpp` line 79; `src/xml.h`.

### D02 · High · Buffered scheduling defeats streaming decoders

**Traced.** `decode_image_part` selects a streaming video decoder before reading the file. During ordinary folder navigation the foreground loader has already called `read_file`, which sizes a vector to the entire file and fills it. The background reader does the same for neighbours. The poster decoder then opens the movie by path. A multi-gigabyte video can therefore cost a multi-gigabyte allocation and read merely by being next to the photo on screen. The first image opened from the command line takes a different route.

Make the decoder registry's input decision available before the loader allocates. A job can carry either a path for a streaming decoder or a shared byte buffer for a buffered decoder. Foreground and prefetch should consume that same decision. Keep the foreground lane and its priority.

**Acceptance.** Open a large video directly, navigate onto it, and prefetch it from an adjacent photo. Instrument `read_file` and verify zero whole-video reads in all three cases. A buffered RAW should still reuse its read for preview and develop. Sources: `src/loader.cpp` lines 163, 224, 283; `src/decoders/decoder.cpp` lines 161, 360; `src/decoders/video_poster.cpp`; `src/decoders/video_poster_mf.cpp`.

### D03 · High · Exporters can silently choose a different version of the image

**Traced.** For a deep image, `App::edited_image` returns the same object after materialising display pixels. That object contains both the edited display pixels and the original high-depth samples. The PNG encoder takes RGB(A)8, the DDS floating layouts read `image.source`, and the plugin bridge chooses between the two according to negotiated depth. For an exposed HDR image, selecting a floating DDS layout can therefore restore the original sample values in the saved file. The command-line exposure step also changes the rendered buffer while retaining the source.

Make export resolve the edit state at the requested precision. Give an encoder an immutable pixel input with explicit sample type, colour interpretation and alpha convention. Preserve access to original samples for an identity edit, and keep the original-file lossless JPEG transform explicit. Baking everything into the eight-bit display buffer would lose an existing feature.

**Acceptance.** Save one float ramp with an identity edit and at +1 EV through an eight-bit encoder, a float DDS layout, and a high-depth plugin. Decode the outputs and check values in their respective transfer functions, including values above white. Sources: `src/ui/app.cpp` (`edited_image`); `src/pipeline.cpp` (exposure step); `src/encoders/dds.cpp` line 106; `src/plugins.cpp` line 462; `src/encoders/encoder.cpp` line 108.

### D04 · High · An asynchronous render still reads the live editable image

**Traced.** `request_retone` captures the current linear/source pointers, dimensions indirectly through the image, the edit tables, and the EV. The worker then calls `image->retone_buffer` on the live object. Crop assigns a new image into that same object, and orientation replaces its buffers and dimensions. The completion callback checks whether its inputs are current **after** the worker has read them. Keeping old allocations alive cannot make a concurrent read of the live fields safe. The detached worker also reaches `ui_` through `this`, while the destructor waits for listing, previews and saves.

Build a small render input on the UI thread containing dimensions, immutable sample ownership and edit parameters. Let the worker read only that input. Publish through a completion owner that can be closed, with a revision check at publication. Apply the same audit to save workers receiving a shared mutable image. The existing loader's owned workers and the view's weak completion guard are useful local precedents.

**Acceptance.** On CPU rendering, drag exposure on a large RAW while rotating, cropping, switching image and closing the window. Run with a race detector where available. Verify that stale results are discarded and shutdown cannot post through a destroyed owner. Sources: `src/ui/app.cpp` lines 1601, 1753, 1775 and `App::~App`; `src/image.cpp` lines 465, 879; `src/ui/app_save.cpp` lines 214, 327.

### D05 · High · File replacement depends on the entry point and shares staging names

**Traced.** GUI Save As uses `platform::write_replacing`. The `--process` save and its `--convert` alias hand the destination directly to the encoder or lossless writer. A PNG/JPEG encoder opens that path with `wb`, so a later failure can destroy a pre-existing output. The GUI helper always stages at `path + ".ifrit-partial"`. Two writers to the same destination can truncate or rename each other's pending write. Save previews likewise share a process-independent filename based on a per-process generation number.

Give each write an exclusively reserved staging name. Route all final saves through one commit-to-destination operation, including the lossless branch. Let each frontend retain its own option defaults and error presentation. Preview files need unique ownership and cleanup but need no final commit. Check encoder close errors before committing.

**Acceptance.** Inject an encoder failure after writing a prefix, save over the input via CLI, and run two processes saving and previewing simultaneously. Existing output must survive failure, successful output must be one complete result, and each process must clean up only its own files. Sources: `src/platform.cpp` line 38; `src/pipeline.cpp` lines 152–184; `src/probe.cpp` line 83; `src/ui/app_save.cpp` lines 221, 331; `src/encoders/encoder.cpp` lines 110, 192.

### D06 · High · Cache charging describes an older representation of an image

**Reproduced.** A 16×16 RGBA float source with its display buffer was charged 1,024 bytes. The two buffers alone held 5,120 bytes. `image_bytes` counts display pixels, linear RAW data, several mosaic payloads, and animation frames. It omits `source->data` and the display mip chain. There is a separate accounting hole when a prefetched full decode is adopted into an already cached preview. That path returns immediately after mutating the cached object, leaving the old insertion charge in the slot. Prefetch distance also uses this estimate.

Keep the estimate next to the types that own the large buffers, and make replacement or growth of a cached representation update the charge and trim the LRU. Define whether shared allocations are charged per entry or once across entries. A conservative documented estimate is enough to make eviction predictable.

**Acceptance.** With a small budget, insert deep images with mips and promote a cached preview to a large final image. Check the total, eviction order and the case where the promoted image alone exceeds the budget. Sources: `src/cache.cpp` lines 16, 56; `src/image.h` (`SourceData`, `Image::Mip`); `src/ui/app.cpp` line 1263 and `schedule_prefetch`.

### D07 · Medium · Editing reconstructs file identity field by field

**Reproduced.** Cropping page 2, mip 1 of a four-page, three-level image yielded page 0, mip 0, one page, one level and an empty label. `Image::crop` constructs a new `Image` and copies a long list of properties. `App::crop_to_selection` assigns it back, and the page/mip commands consult the reset counts to decide whether navigation is possible. `Image::adopt` has a second, different field-transfer list for preview promotion.

Group source-file and part identity together. Have crop replace the raster and geometry while retaining that identity. Preview promotion should replace the decoded representation and explicitly merge preview-only metadata and replay edits. This is a bounded first cut into `Image`; it can be validated independently of a renderer redesign.

**Acceptance.** Crop a nonzero page and mip, check its label and navigation, then revisit it. Check crop and rotation on a RAW preview before its final decode lands. Sources: `src/image.cpp` lines 598, 738; `src/image.h` (`ImagePart`, `Image`); `src/ui/app.cpp` (`crop_to_selection`, `step_page` and the adjacent level-navigation code).

### D08 · Medium · A file and a part of a file are both represented as a path string

**Reproduced.** The key format uses ASCII unit separator followed by decimal page and mip numbers. A real Linux filename containing that byte is valid. Passing the default part of the filename consisting of `legal`, byte 31 and `2.1` through the helpers resolved to the file `legal`. Even for ordinary filenames, file-change handling erases the bare key while the cache may hold several part keys. The mtime check on a later get provides partial protection, but erasing a file and erasing a part are distinct operations.

Use a value containing path and `ImagePart` through the loader, cache and selection. Format it only for traces. Expose file-wide invalidation for watcher and settings events. A typed key also removes parsing from the foreground, prefetch and result paths.

**Acceptance.** Load the control-character filename on Linux, cache two pages of one ordinary file, invalidate that file and check both pages. Sources: `src/image.cpp` lines 540–571; `src/loader.cpp` lines 137, 165; `src/cache.cpp` line 75; `src/ui/app.cpp` line 1019.

### D09 · Medium · Startup and browsing implement the first decode twice

**Traced.** `FirstImage` waits for settings, chooses preview versus full decode, builds mips and disposes of the thread-local input. The foreground loader implements those decisions again. History shows the quick-RAW decision changed both paths in `e055955`, then the preview-size limit had to reach startup separately in `527e9e1`. On a successful preview the loader carries its existing file bytes into develop. Startup drops them and later calls `request_develop` with a filename. The early start before toolkit creation has a concrete latency benefit.

Extract the operation that produces the first displayable result and optional continuation. Pass it a file/part, fit bounds and input policy from D02. Startup can still begin before the toolkit exists, and the foreground worker can still promote its continuation ahead of prefetch. Keep thread-local compatibility at decoder boundaries until callers can pass a request directly.

**Acceptance.** Trace cold open and folder navigation for an ordinary JPEG, a quick RAW, a preview-first RAW and a movie. Check first-frame timing, bytes read and preview-to-final order. Sources: `src/first_image.cpp` line 6; `src/loader.cpp` lines 144–193; `src/ui/app.cpp` line 923.

### D10 · Medium · Failed GPU materialisation looks like a usable CPU image

**Traced.** For a GPU-developed mosaic, `pixels_for_cpu` sets a status message if readback fails, then returns void. `edited_image` still hands back the image. The clipboard path supplies its dimensions and pixel pointer to Rapport, and print/save also consume the result. A mosaic can legitimately have dimensions and an empty pixel buffer. This makes checking availability each caller's responsibility without giving it a success result.

Make CPU materialisation return either a valid raster view with its storage owner or an error. Clipboard, print, compare and save should perform their side effect only after acquiring one. Route invalidation after exposure, white balance, lens changes and develop controls through one operation with an explicit revision.

**Acceptance.** Force GPU export/readback failure after successful display. Copy, print and save should report the failure, leave destinations untouched and allow retry. Sources: `src/ui/app.cpp` lines 640–685; `src/ui/app_print.cpp` line 15; `src/ui/app_develop.cpp` line 299; `src/ui/gpu_view.cpp` (`render` and readback path).

### D11 · Medium · Geocoding splits one pending request across three containers

**Traced.** A key can appear in the queue, the waiting-callback map and the disk-backed result map. `set_provider` clears only the queue. If A is in flight and B queued, changing settings can leave B in the waiting map with no queue entry. The next lookup of B sees “already queued” and never starts it. Independently, HTTP failure and a valid empty lookup both call `resolve(key, "")`, which persists the result. A temporary outage can suppress future lookups across restarts.

Give each pending key one owner for its callbacks and scheduling state. Define how provider change cancels or reschedules outstanding keys. Cache a confirmed miss separately from transport or parse failure. Preserve the request spacing and the existing on-disk format or give it an explicit migration policy.

**Acceptance.** Queue two places, change provider before the first completes and revisit the second. Simulate a transient 503 followed by a success, including a restart. Sources: `src/geocode.cpp` lines 48, 92, 107, 198, 263; `src/ui/app.cpp` (settings completion).

### D12 · Medium · Lens database installation can lose the last valid copy

**Traced.** The installer validates the new archive before installation, but uses a shared `.new` directory, deletes the installed directory, then renames the staged directory. Rename failure or interruption between deletion and rename loses the usable on-disk database. Concurrent viewer processes can remove each other's staging directory. This concerns the cached database available at the next launch, including when offline.

Give installations unique staging directories and serialise publication between processes. Preserve a recoverable valid generation until the replacement is durably selected, while respecting the lensfun-compatible directory layout. Scavenge abandoned stages only when their ownership is clear.

**Acceptance.** Force rename failure with a valid cache installed, interrupt between stages, then restart offline. Run two simultaneous installers. At least one complete valid generation must always be recoverable. Source: `src/lens_store.cpp` lines 244–285.

### D13 · Medium · The RAW readout type no longer describes its alternatives

**Traced design debt.** `RawMosaic::Stream` can mean prepared JPEG, packed samples, encoded blocks, or an already-decoded array. Only packed and block forms need the external file owner. Plane layout applies to a subset of decoded arrays. Slices, tiles and quads have different applicability again. The decode producer, GPU consumer and cache estimator each have to know these combinations. Separately, `quick` opens and probes several of the same RAW layouts that `decode_mosaic` prepares. Their purposes differ, but both encode knowledge of which readouts can proceed directly to the GPU.

Represent the four payload alternatives explicitly, keeping source-byte ownership with the payload that references it and common sensor geometry outside. Let preparation report whether it has a cheap path and reuse its header work when it does. Keep expensive full preparation out of the quick decision. Preserve the CPU and LibRaw fallback order and the existing specialised kernels.

**Acceptance.** Exercise packed, block, JPEG and predecoded inputs, compare the unpacked samples and black levels with the existing RAW check modes, and measure time to first display. Sources: `src/image.h` line 63; `src/decoders/raw.cpp` lines 1517, 1546, 2026; `src/ui/gpu_view.cpp`; `src/cache.cpp` line 16.

### D14 · Low · Texture decoders still implement parts of the shared texel layer locally

**Traced duplication.** DDS and KTX each have a private `Masks` description and a masked decode loop. KTX handles row alignment, endian conversion and associated alpha. Both need the same mask expansion and ARGB storage convention. BCn, ETC, ASTC and KTX also contain local alpha multiplication, clamping and packing that already exist in `texel.h::argb`. CPU sRGB transfer functions recur in image rendering, plugins and RAW setup despite the existing typed functions. Some variants deliberately clamp or sanitise NaNs.

Move masked layouts alongside `WideTexels` and use the shared ARGB packer at the point channels have been decoded. Make the open-range transfer curve shared and leave clamping and quantisation explicit at callers. Keep format detection, compressed block algorithms and container validation local.

**Acceptance.** Compare output byte-for-byte for odd widths, aligned rows, both byte orders, alpha-only textures, and straight/associated alpha at 0, 1, 254 and 255. Check HDR values above 1 and NaNs separately. Sources: `src/decoders/dds.cpp` line 38; `src/decoders/ktx.cpp` lines 49, 291; `src/decoders/texel.h` lines 19, 59; `src/image.cpp` line 206; `src/plugins.cpp` line 419.

### D15 · Low · The Rapport migration left duplicate platform discovery and an obsolete GPU design description

**Traced.** Application startup, translations and Settings use `rap::sys::executable_path`. Format-plugin discovery and desktop registration ask IfritView's independent Win32/Linux implementation. This gives two sets of path encoding, buffer sizing and failure semantics for locating the same installed application. The GPU design notes still describe an IfritView-owned EGL context and a possible future switch to Vulkan. The build and pinned Rapport instead implement HLSL on Rapport's Vulkan/D3D11 devices. The old account is especially misleading when deciding which reviewer owns a driver or presentation change.

Use one executable discovery routine, preserving any required fallback at the caller. Update the GPU document to distinguish the shipped device and shader ownership from outstanding display goals. Keep IfritView-specific policy such as cache locations and image edits in IfritView.

**Acceptance.** From a Unicode install path, check startup, format plugin discovery, translations and registration. Trace both GPU diagnostic commands to the rendering path they actually exercise. Sources: `src/platform.cpp` line 112; `src/plugins.cpp` line 946; `src/i18n.cpp` line 15; `CMakeLists.txt` line 367; `docs/gpu-pipeline.md` lines 34–65; pinned Rapport `core/src/sys.cpp`, `core/src/gpu_vulkan.cpp`, and `core/src/gpu_d3d11.cpp`.

### D16 · Medium · Folder prefetch and individual codecs each budget the same cores

**Traced.** The loader starts roughly four background workers per five hardware threads. A worker doing a full RAW decode can enter `parallel`, which by default starts another thread per hardware thread for a sufficiently large loop. Canon cRAW has nested tile, plane and subband parallelism with explicit counts. The loader controls how many files are active while the codecs independently control threads per file. This admits far more runnable threads than the loader's apparent budget. Actual latency and memory impact depend on the camera, machine and browsing pattern.

Pass a concurrency allowance with a decode. Let an otherwise idle foreground decode use the machine, and divide the allowance among concurrent background files. Start with the existing helpers and codec runner parameters before considering a shared executor.

**Acceptance.** Measure cold foreground decode and sustained browsing separately for CR3, tiled DNG and an ordinary still. Record time to first pixels, throughput, peak threads and memory. Preserve the single-file fast path. Sources: `src/loader.cpp` lines 23, 243; `src/parallel.h` line 12; `src/decoders/crx.cpp` lines 1060–1100; `src/decoders/dngtiles.cpp` line 126.

### D17 · Medium · Two buffered decoders copy the entire input a second time

**Traced.** WebP and JPEG XL contain the same private `read_file(path, vector&)`. Each obtains the decoder's already-owned file bytes and copies the whole range with `data.assign`. The libraries then receive a read-only pointer and size. The copy survives for the duration of decoding and adds one compressed-file-sized allocation per active job. This is independent of D02's streaming issue and can be fixed on its own.

Use the existing bytes directly and retain their owner for the longest-lived decoder or demuxer referencing them. Check animation iteration and all decoder destruction paths before releasing the input.

**Acceptance.** Compare pixels, frame metadata, and peak allocations for a large JXL, a still WebP and an animated WebP during prefetch. Sources: `src/decoders/jxl.cpp` lines 22, 72; `src/decoders/webp.cpp` lines 21, 34, 52; `src/decoders/decoder.cpp` line 180.

### D18 · Medium · An animation edit applies to one display buffer and vanishes on the next frame

**Traced.** Rotating an animated image transforms its current pixels and dimensions. The stored animation frames retain their original layout. The next playback tick tests only byte count and copies one of those frames into the transformed image. A 90-degree rotation preserves byte count, so a non-square frame gets interpreted with the rotated width and stride. A flip is simply overwritten. Crop behaves differently again because constructing the cropped image drops the animation.

Define the editing semantics for a sequence. If edits follow playback, store geometry with the sequence and apply it when presenting or exporting a frame. If editing extracts a still, make that operation explicit and end playback consistently for both rotate and crop.

**Acceptance.** Use an asymmetric two-frame GIF or WebP, rotate while playing, pause, copy, resume, then crop. Verify geometry, pixels and playback state across the second tick. Sources: `src/ui/app.cpp` (`transform`); `src/image.cpp` lines 598, 879; `src/ui/app_playback.cpp` lines 234–257.

### D19 · Medium · Shuffle order stores positions in a directory that can change underneath it

**Traced.** Shuffle captures the folder's current integer indexes when playback starts. The directory watcher can insert, rename or remove files while the slideshow is active. A saved index can then select a different photo or exceed the new count. The slide timer is one-shot. If `Folder::go` rejects the next index, the shuffle branch neither loads another image nor arms the next timer, so progression can stop.

Track shuffled entries by file identity and decide explicitly when newly discovered files join a cycle. Resolve or skip vanished entries when advancing, with a defined stop condition when there are too few remaining.

**Acceptance.** Start shuffled playback, remove a future slide, rename another and insert a file before the current one in sort order. Check the remaining files are visited and that the timer continues or playback ends explicitly. Sources: `src/ui/app_playback.cpp` lines 36, 98, 142; `src/folder.cpp` (`go`); `src/ui/app.cpp` (`folder_changed`).

### D20 · Medium · The sorted directory loses its sort keys as soon as it becomes live

**Traced.** The full scan computes one collation key per entry, sorts pairs of key and name, then discards the keys. Live insert and rename recompute locale-aware keys for successive existing paths in a linear search. These updates run while handling the watcher batch on the UI thread. Full scan orders ties by name, whereas incremental insertion checks only whether an existing key is greater. A rescan can therefore also reorder distinct names with equal collation keys.

Keep the ordering rule and entry representation with the live list. Retained keys and a binary search on the same pair used during scanning remove repeated collation. Rebuild the list when a setting changes its ordering. Vector insertion still moves elements, so measure batches before adding another index.

**Acceptance.** Feed inserts and renames into a large recursive listing, then compare its order with a fresh scan. Measure main-thread time for a watcher burst and check current-file preservation. Sources: `src/folder.cpp` (`sort_key`, `list_images`, `add`, `rename`); `src/ui/app.cpp` (`folder_changed`).

### D21 · Low · GPU statistics have two independent descriptions of the buffer layout

**Traced.** Shaders give the count, peak, minimum, maximum and P99 words names and define the buffer length as 1101 words. The C++ code allocates 1101 words in two places and reads literal offsets 1096–1100. Moving a statistic can leave both shaders valid and C++ compiling while the details panel silently reports the wrong quantity. The neighbouring constant-buffer structs already have size assertions, which provide a precedent.

Keep word offsets and lengths in a small shared definition suitable for the shader and C++ compilers. Leave readback, units, and rendering code on their respective sides. Treat CPU/GPU algorithms that intentionally provide independent reference outputs separately from this duplicated storage contract.

**Acceptance.** Read back a synthetic distribution with distinct minimum, maximum and percentiles. Verify the UI fields under manual and automatic exposure and check shader compilation on both targets. Sources: `src/shaders/view.hlsli` lines 67–74; `src/shaders/peak.hlsl`; `src/ui/gpu_view.cpp` lines 212, 236–244, 1223.

### D22 · Medium · The fullscreen overlay infers image semantics from presentation fields

**Traced.** The overlay recognises shot properties by searching groups and display strings such as “Focal length”, “Focal Length”, “Shutter” and “Exposure Time”. The EXIF reader supplies library-provided tag titles and the RAW reader supplies its own labels. Independently, the overlay decides that a tonable image is “developed” when `linear` exists and “HDR” otherwise. A GPU-developed RAW can have `mosaic` alone, so it enters the HDR branch solely because of where it was developed.

Give the handful of fields used for decisions stable semantic accessors or identifiers. The full metadata pane can retain arbitrary format-specific rows and translated labels. Derive the RAW/HDR description from the source and edit state once for presentation.

**Acceptance.** Compare overlay content for a JPEG carrying EXIF and a RAW carrying the same shot fields. Compare CPU/GPU RAW labels, an actual HDR image, and a non-English language setting. Sources: `src/ui/app_metadata.cpp` (`overlay_sections`, especially lines 295–347); `src/decoders/exif.cpp` line 21; `src/decoders/raw.cpp` lines 1958–1965; `src/image.h` (`tonable`).

### D23 · Medium · Windows packaging maintains a second inventory of linked components

**Traced.** The notice generator enumerates static dependencies in its own Python list. Windows' vcpkg manifest includes libjxl and zstd, and CMake links their targets when found. Neither port appears in the generator's list. Its missing-file check iterates only listed ports, so it cannot discover either omission on a build that links them. This is a build-manifest discrepancy. Identifying the full affected release set requires inspecting the shipped binaries or link records.

Reconcile the package notice inputs with the resolved target dependencies during packaging, retaining human descriptions and exceptional licence-file locations as overrides. Distinguish host tools from target libraries.

**Acceptance.** Compare a full static Windows link against the generated notice sections. Enable or disable a codec and verify the resulting inventory follows the executable. Sources: `vcpkg.json`; `CMakeLists.txt` lines 282, 313–315; `.github/ci/third-party-notices.py` lines 25, 80; `.github/ci/build-windows.ps1` (package stage).

### D24 · Medium · Windows upgrade migration deletes the source before verifying the copy

**Traced.** The installer explicitly preserves user-supplied format plugins during an upgrade from the older `bin/plugins` layout. It attempts a silent copy into the new plugins directory, then unconditionally removes the old `bin` tree. If a copy fails, the upgrade still reaches deletion of the directory containing the user's original plugin. The migration logic is a continuing compatibility obligation for existing installs.

Make migration failure observable and keep original user files until the migration has succeeded. Specify collision behaviour when both layouts already contain a file of the same name.

**Acceptance.** Upgrade a legacy installation with a user plugin, inject a destination write failure, then retry. Verify that the original survives failure and that retry produces one usable plugin. Source: `data/windows/ifritview.nsi` (install section immediately after `Call CloseRunning`).

### D25 · Low · Distribution builds maintain separate copies of the same smoke suite

**Traced duplication.** Debian/Fedora CI and the Arch package check separately stage a 2×2 PPM, generate random 64×48 data, run shrink to 23×17, write a tiny farbfeld fixture, and validate SPIR-V. Neither shrink invocation asserts expected output pixels. A regression can therefore need two test edits and still pass if it returns the wrong image successfully.

Keep one headless smoke script accepting a build directory and a scratch directory. Let each package keep its own installation and dependency checks. Use deterministic input and assert dimensions and pixels where the pixel result matters. Keep the standalone fixture generators independent of the code they help test.

**Acceptance.** Run the same pixel and plugin checks from both package paths. Deliberately perturb one expected pixel and verify both jobs fail. Sources: `.github/ci/build.sh` lines 31–51; `packaging/arch/PKGBUILD` (`check`); `.github/ci/build-arch.sh`.

## Coverage and boundaries worth preserving

| Area swept | Evidence and judgement |
| --- | --- |
| Startup, folder watching, navigation, loader and cache | Traced first open, foreground decode, prefetch, preview adoption, part selection and invalidation. Preserve foreground priority and the separation between listing a directory and adopting it on the UI thread. D02, D06–D09. |
| Decoders, format plugins, RAW and texture formats | Examined registry ordering, streaming, fallback, plugin adapter, RAW preparation and shared texel operations. Keep the C plugin ABI and the builtin-before-fallback ordering. The individual camera bitstream algorithms merit corpus-based review when changed. D02, D03, D13, D14. |
| Editing, CPU rendering, GPU view and diagnostics | Followed edits into retone, GPU readback, display and export. CPU reference arithmetic and shader implementations serve distinct purposes. The headless `--gpu-render` already exercises the view renderer. Share input semantics and test parity before consolidating implementations. D03, D04, D07, D10, D15. |
| Native UI, settings, compare, save preview and playback | Examined layout/refit state, input dispatch, compare/save ownership, playback interfaces and the async completion paths. Fractional scaling and asynchronous native layout justify some refit complexity. Preserve device-pixel sizing and validate on real backends before removing retries. GStreamer and Media Foundation have materially different playback lifecycles. |
| Metadata, sidecars, colour, geocoding and lens data | Traced XMP input and output, metadata filtering, declared colour space handling, asynchronous lookup and database installation. Keep format-specific EXIF and IPTC editing separate. The duplication that matters is two parsers for the same XMP. D01, D03, D11, D12, D14. |
| CLI, output formats, OS integration and Rapport | Traced process/convert, lossless JPEG, Save As, plugin layouts, executable discovery and toolkit dispatch. `--convert` already delegates to `--process`. Native save dialogs and CLI option parsing can stay independent over a shared writer. D03–D05, D10, D15. |
| Build, packaging, localisation and verification | Inspected CMake targets and optional backends, Linux package/smoke scripts, Windows staging/import checks and translation discovery. The Linux smoke script probes files, runs shrink and validates compiled shaders. The shrink smoke checks command completion without comparing resulting pixel values. Add behavioural checks at the affected boundaries as backlog items land. |

## Suggested order of work

### Changes merged to master

The owner merged PRs #2–#6. All five are present on `master` at `b2b1401`. D17 and D21 are addressed. D14, D15 and D25 have follow-up work listed below.

| Finding | Merged change | Remaining scope |
| --- | --- | --- |
| D15 | [#2 documents the shipped GPU pipeline](https://github.com/TurkeyMan/ifritview/pull/2) | Executable discovery still has two implementations. |
| D14 | [#3 shares the ARGB packer across four texture decoders](https://github.com/TurkeyMan/ifritview/pull/3) | Masked layouts and transfer curves require separate equivalence checks. |
| D17 | [#4 decodes WebP and JPEG XL from the buffered input](https://github.com/TurkeyMan/ifritview/pull/4) | Covers both copies identified in this finding. |
| D21 | [#5 shares the statistics layout between C++ and HLSL](https://github.com/TurkeyMan/ifritview/pull/5) | Covers the word offsets and allocation length. |
| D25 | [#6 shares the Linux package smoke suite](https://github.com/TurkeyMan/ifritview/pull/6) | Deterministic fixtures and pixel assertions remain separate work. |

### Validation and follow-up

1. **Protect outputs and user choices.** Fix D01 first because exported files can retain a location. D05 and D12 can proceed independently. Give each a failure-path regression before changing the implementation.
2. **Protect opening and browsing large files.** D02 and D06 are bounded changes with observable I/O and memory acceptance criteria. D08 supplies a better key for file-wide invalidation. Fold D09 into the resulting loader work once cold-start timing is recorded.
3. **Give edits and exports a defined input.** Start with D07's crop regression and D10's failure result. Use D04's immutable render inputs as the basis for D03's precision-aware export. This sequence allows individual changes to be reviewed without replacing the entire image model at once.
4. **Retire duplicate policy.** D11 is independent. Schedule D13 with camera fixtures and timing data, D14 with pixel comparisons, and D15 alongside the next platform or GPU documentation change.
5. **Group the follow-through by owner.** Address D23 and D24 before the next affected Windows package. Include D17 with buffered input work. Measure D16 before choosing a concurrency policy. Exercise D18 alongside the image geometry changes, and D19 and D20 together against one changing directory. D21, D22 and D25 can each land independently.

## Verification record

**Coverage reconciliation.** The baseline contains 267 tracked entries including the Rapport gitlink. All 196 entries under `src` were mapped by responsibility, declarations and local dependencies: 66 core, 65 decoder, 32 shader, 22 UI, six encoder and five metadata files. The table above records the deeper call-path reviews. D16–D25 extend those paths to nested parallelism, codec input ownership, animation, live folder ordering, shuffle, shader readback, overlay semantics and release engineering. This is module-level design coverage. An inventory entry alone supplies no correctness verdict on its implementation.

**Outside the source tree.** The sweep included the public plugin header and farbfeld example, CMake and build entry points, the CI scripts and workflow, Windows installer and manifests, Arch packaging, vcpkg manifest and overlay, fixture and translation tools, desktop metadata, translations and project docs. The plugin header explicitly distinguishes all accepted input suffixes from the canonical Save As suffix. The example and adapter respect that distinction. The two fixture generators cover different library-dependent and standalone formats. The vcpkg overlay addresses upstream build differences. Those similarities do not justify merging their implementations. Icons and localisation catalogues were considered as build/runtime inputs; their visual and linguistic quality was outside scope. The 21 third-party entries were considered at their integration, attribution or pin boundary.

The review inventoried 196 tracked files under `src`, followed representative user flows across subsystem boundaries, compared repeated code, consulted relevant history, and checked the Rapport pin. The source locations above refer to the recorded revision. Two temporary Linux harnesses compiled the production XML/export and image/cache code. They reproduced D01, D06, D07 and D08. The reviewed translation units used by those harnesses were identical in the local checkout and the reviewed revision. The other findings are source-based. This sweep did not run a complete application build, the camera corpus, sanitizers, GPU hardware comparisons or the native GUI matrix. Vendored codecs and Rapport internals were outside the algorithm-by-algorithm scope.
