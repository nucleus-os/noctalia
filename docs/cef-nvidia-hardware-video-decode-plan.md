# CEF NVIDIA Hardware Video Decode Plan

Last updated: 2026-07-18

## Decision

Enable hardware decoding for Apple Music's animated artwork and other eligible
CEF video on the compositor-selected NVIDIA GPU.

The implementation will use Chromium's existing Linux VA-API decoder with the
source-built `nvidia-vaapi-driver`, which translates VA-API decode operations
to NVDEC. Decoded video remains inside Chromium's GPU media and SharedImage
pipeline and is composed by Skia Graphite over Dawn Vulkan.

This is a media-pipeline optimization, not another Noctalia renderer:

- CEF's final page rendering remains Graphite/Dawn/Vulkan-only.
- No CPU-painted CEF frame or copied page-rendering fallback is introduced.
- Chromium may still use software codec decoding for an unsupported codec,
  protected stream, driver failure, or other media-specific fallback. That is
  distinct from CPU-rendering the CEF page.
- Hardware decode must not be claimed merely because VA-API initializes. The
  selected decoder and sustained decoded-frame flow must be verified.

Start with the conservative single-buffer NV12 export path. Treat native
per-plane export as the likely long-term NVIDIA path after its complete
descriptor, pixel, synchronization, reuse, and lifetime contract passes.

## Current findings

### What Apple Music renders

Apple Music animated album and playlist artwork is not an animated image,
canvas, WebGL surface, or CSS animation.

The site creates an `amp-ambient-video` custom element whose implementation
renders a muted, inline, looping HTML `<video>`. On Linux it uses HLS.js and
Media Source Extensions to feed an Apple `.m3u8` stream to that element.

The inspected artwork stream is 24 fps SDR HLS with H.264/AVC and HEVC
variants. Chromium will ordinarily choose an AVC variant on this Linux build.
HLS.js performs manifest and segment handling in JavaScript, but that does not
require software video decoding: Chromium can still pass the compressed AVC
samples to a hardware decoder.

### Current CEF capability

The optimized CEF M151 build already has:

```text
use_vaapi=true
proprietary_codecs=true
ffmpeg_branding="Chrome"
enable_platform_hevc=true
```

No CEF rebuild is required merely to compile in `VaapiVideoDecoder` or H.264
support.

Chromium deliberately disables NVIDIA VA-API at runtime in two places:

- `VaapiWrapper` skips `nvidia-drm` unless `VaapiOnNvidiaGPUs` is enabled.
- the Vulkan media client accepts only Intel by default unless
  `VaapiIgnoreDriverChecks` is enabled.

The current Noctalia command line enables neither feature, so Apple Music
video is currently decoded in software even though Viz and Noctalia render on
the GPU.

### Driver status

Ubuntu's `nvidia-vaapi-driver` package is version `0.0.14`. It exposes the
RTX 4070 Ti's H.264, HEVC, VP8, VP9, and AV1 decode profiles successfully, but
it predates the recent Chromium-oriented direct-backend work.

The local source checkout is:

```text
~/Developer/nvidia-vaapi-driver
03bb5a0c082493f95f2cd54ffd31dbfa8c7cbe7d
v0.0.17-54-g03bb5a0
```

Relative to `v0.0.14`, that source contains work directly relevant to this
integration:

- Chromium stream-format transition handling;
- waiting for display-surface resolution before export;
- backing-image reuse and bounded detached-image caching;
- reduced per-frame host synchronization and allocation;
- single-buffer and per-plane direct-backend export paths;
- plane-specific block-height and modifier fixes;
- chroma-corruption fixes;
- cleanup of failure, rollback, and resolver-thread lifetimes.

The apt package has been removed. No NVIDIA VA-API driver is presently
installed, so setting VA-API environment variables alone currently does
nothing.

## Two distinct GPU boundaries

The implementation must not conflate the decoder-to-Viz handoff with the
existing CEF-to-Noctalia handoff.

```text
Apple HLS
  -> HLS.js / Media Source Extensions
  -> compressed AVC samples
  -> Chromium VaapiVideoDecoder
  -> nvidia-vaapi-driver
  -> NVDEC
  -> decoded NV12 DMA-BUF SharedImage
  -> Chromium VideoResourceUpdater / TextureDrawQuad
  -> Viz Skia Graphite / Dawn Vulkan
  -> complete ARGB CEF root frame
  -> CEF offscreen DMA-BUF output queue
  -> Noctalia Vulkan import
  -> Noctalia Graphite sampling
  -> Wayland presentation
```

The first boundary transports a decoded video frame, commonly multiplanar
NV12. The second transports the already-composited CEF root frame as ARGB.

The animated-art yellow flash may originate at the first boundary and then be
faithfully captured in the second. Hardening only `CefGpuFrameBridge` cannot
repair a video frame that was already sampled with stale or misinterpreted
chroma inside Viz.

## NV12 export models

NV12 contains:

- a full-resolution Y plane carrying brightness;
- a half-resolution interleaved UV plane carrying color.

### Single-buffer export

```text
one DMA-BUF allocation
  offset 0: Y
  later offset: UV
```

Advantages:

- one allocation identity;
- simpler lifetime and frame-level synchronization;
- the conservative Chromium compatibility path;
- a useful first hardware-decode baseline.

Costs and risks:

- both planes must advertise one compatible modifier;
- chroma begins at a non-zero tiled offset;
- NVIDIA block-height differences must be reconciled correctly;
- packing can hide plane-specific layout assumptions in importers.

### Per-plane export

```text
DMA-BUF object 0: Y
DMA-BUF object 1: UV
```

Advantages:

- preserves each plane's natural dimensions and storage;
- allows plane-appropriate tiling, block height, and modifier;
- avoids importing tiled chroma from a non-zero offset;
- maps naturally to explicit Dawn/Vulkan plane views.

Costs and risks:

- both objects must share one immutable frame identity;
- one producer completion dependency must cover both objects;
- both objects must remain alive until the last consumer read completes;
- descriptor ordering, modifiers, color metadata, and view aspects must agree;
- stale or wrong UV data can produce a recognizable image with severe
  yellow/green corruption.

The current driver source defaults multiplanar YUV to per-plane objects.
`NVD_SINGLE_BUFFER=1` selects its packed compatibility path. The README's
claim that multiplanar export is always single-buffer is stale relative to
the source at the pinned commit; the implementation, not that sentence, is
authoritative.

## Required invariants

Every hardware-decoded frame must satisfy all of the following:

1. NVDEC, Chromium's GPU media client, Dawn, Viz, and Noctalia use the
   compositor-selected RTX 4070 Ti.
2. The VA-API surface is not exported until decode or VideoProc resolution
   has completed.
3. The exported DRM format, number of objects, plane ordering, strides,
   offsets, sizes, and modifiers describe the actual allocation.
4. Y and UV belong to the same decoded content frame.
5. The acquire dependency makes all decoder writes to both planes visible
   before Dawn/Graphite sampling.
6. Chromium cannot recycle either plane while Viz may still sample it.
7. Dawn creates the correct per-plane texture views and Skia receives matching
   promise and fulfillment metadata.
8. YUV color-space, range, chroma siting, and bit-depth metadata survive the
   decoder-to-Skia path.
9. A decoder resolution, codec, profile, or output-format transition retires
   old surfaces without aliasing them with the new stream.
10. Failure closes or falls back at the media-decoder boundary without
    corrupting the CEF root-frame transport.

The existing CEF root-output ownership invariants in
`cef-zero-copy-frame-architecture.md` remain independently required.

## Phase 1: build a pinned private driver

Do not overwrite a dpkg-owned system file. Build the source checkout into a
versioned private prefix so rollback is removing a directory and unsetting its
path.

Install the remaining source-build dependencies:

```sh
sudo apt install \
  libffmpeg-nvenc-dev \
  libgstreamer-plugins-bad1.0-dev
```

Configure and build the pinned checkout:

```sh
cd ~/Developer/nvidia-vaapi-driver

meson setup build-noctalia \
  --wipe \
  --prefix="$HOME/.local/lib/noctalia/vaapi/nvidia/03bb5a0" \
  --libdir=lib \
  --buildtype=release

meson compile -C build-noctalia
meson install -C build-noctalia
```

The expected driver directory is:

```text
~/.local/lib/noctalia/vaapi/nvidia/03bb5a0/lib/dri
```

Verify that exact artifact against CEF's render node:

```sh
LIBVA_DRIVER_NAME=nvidia \
LIBVA_DRIVERS_PATH="$HOME/.local/lib/noctalia/vaapi/nvidia/03bb5a0/lib/dri" \
NVD_BACKEND=direct \
NVD_SINGLE_BUFFER=1 \
vainfo --display drm --device /dev/dri/renderD129
```

Acceptance:

- the driver reports the direct backend;
- `VAProfileH264High` advertises `VAEntrypointVLD`;
- the loaded `.so` resolves from the private prefix;
- no system driver file is created or replaced.

Keep the source commit pinned until runtime acceptance is complete. Updating
the driver and changing Chromium integration code in the same run would make
failures ambiguous.

## Phase 2: scope the driver environment to Noctalia

Do not add the driver variables globally to `/etc/environment`.

Set the following in Noctalia before `CefInitialize()` and before CEF spawns
its GPU subprocess:

```text
LIBVA_DRIVER_NAME=nvidia
LIBVA_DRIVERS_PATH=<private-prefix>/lib/dri
NVD_BACKEND=direct
NVD_SINGLE_BUFFER=1
```

The parent process must set them early enough that every CEF subprocess
inherits the same values. Keep the private driver path in one build/install
constant or launch configuration; do not duplicate it across unrelated
services.

Do not set:

- Firefox-only `MOZ_*` variables;
- the NVIDIA 470-only EGL vendor override;
- `NVD_LOG` or statistics variables during normal operation;
- `CUDA_DISABLE_PERF_BOOST` until decode correctness is established.

`CUDA_DISABLE_PERF_BOOST=1` may later be evaluated as an optional power
optimization on the installed NVIDIA 610 driver. It is not part of decode
correctness.

## Phase 3: enable Chromium's NVIDIA decoder gates

Add these CEF features:

```text
VaapiOnNvidiaGPUs
VaapiIgnoreDriverChecks
```

`AcceleratedVideoDecoder` is already enabled by default in this `use_vaapi`
build. Keep it explicit only if doing so improves the clarity of the final
command line; do not add redundant switches solely as ceremony.

Do not initially add broad switches such as:

```text
--ignore-gpu-blocklist
--disable-gpu-driver-bug-workarounds
```

Only add a broader override if the observed Chromium GPU feature status proves
that a separate blocklist prevents decoder creation. The two narrow feature
gates correspond to the source checks already identified.

The Vulkan media-client path also requires ANGLE Vulkan and Vulkan GPU
information. Those are already part of the mandatory CEF Graphite/Dawn/Vulkan
configuration and must remain on the same selected GPU.

## Phase 4: prove actual hardware decoder selection

VA-API initialization is necessary but insufficient. Prove all of:

1. Apple Music chooses an AVC artwork variant.
2. Chromium instantiates `VaapiVideoDecoder`, not `FFmpegVideoDecoder`.
3. The NVIDIA driver receives decode submissions and exports decoded frames.
4. Frames continue flowing during sustained animation.

Use temporary, narrowly scoped evidence:

- a driver log through `NVD_LOG`;
- Chromium media logging around decoder selection;
- NVIDIA decoder-engine/process activity when the open-kernel driver reports
  it reliably.

Do not retain verbose per-frame diagnostics in production. A single startup
log stating the selected decoder may remain if it is cheap and comes from an
authoritative selection point.

If VA-API advertises H.264 but Chromium falls back:

1. inspect the decoder initialization error;
2. inspect the requested output format and frame-pool allocation;
3. verify the selected DRM render node;
4. verify Vulkan/ANGLE feature requirements;
5. fix the exact rejection rather than enabling every GPU override.

## Phase 5: establish the single-buffer correctness baseline

Keep `NVD_SINGLE_BUFFER=1`.

Test:

- multiple animated album and playlist covers;
- navigation between static and animated artwork;
- repeated HLS loop boundaries;
- panel close/reopen;
- page reload and browser recreation;
- panel resize and fractional scaling;
- simultaneous audio playback and artwork animation;
- Apple Music login/profile persistence;
- AAC playback and Widevine playback.

Acceptance:

- hardware decode remains selected;
- no yellow, green, red, torn, stale, or partially updated artwork;
- animation cadence remains continuous without pointer movement;
- no CEF GPU-process crash, freeze, or blank recovery;
- no unbounded VA surfaces, DMA-BUF FDs, CUDA allocations, or detached backing
  images;
- CEF root-frame Vulkan validation remains clean;
- interaction and presentation latency do not regress.

Run a sustained animated-art session, not only a short visual check. Stream
loops and surface reuse are important coverage.

## Phase 6: build a real per-plane correctness gate

Do not declare per-plane export production-ready based on descriptor
construction tests alone.

Extend the Chromium/CEF test coverage to create a known NV12 frame with
separate Y and UV DMA-BUF objects and render it through the same
SharedImage → Dawn → Graphite path used by video.

The test must cover:

- known luma and chroma patterns with exact expected RGBA pixels;
- both plane texture-view aspects;
- different legal plane strides and sizes;
- plane-specific modifiers/block heights;
- full-range and limited-range video;
- the color spaces used by Apple artwork;
- repeated frame reuse under producer/consumer fences;
- rapid alternation of chroma values so stale UV is unmistakable;
- resolution and stream-format changes;
- retirement while frames remain in flight;
- allocation/import failure rollback.

A static “the descriptors look right” unit test is insufficient. The gate
must read back rendered pixels and exercise reuse.

Audit the custom Linux Graphite/Dawn path for formats marked
`PrefersExternalSampler()`. That patch deliberately replaces the stock
external-sampler behavior with explicit plane images on Linux. Confirm that:

- the promise texture metadata accepts the fulfilled plane aspect;
- the Y and UV views reference the intended objects;
- begin/end access covers every plane;
- one frame-level dependency covers all planes;
- release occurs after the last Graphite use;
- cached representations cannot combine planes from different frames.

## Phase 7: enable native per-plane export

After Phase 6 passes, remove `NVD_SINGLE_BUFFER=1` and repeat the complete
Phase 5 acceptance run.

Compare the two modes:

| Concern | Single buffer | Per-plane objects |
| --- | --- | --- |
| Allocation identity | One | One per plane |
| Chroma offset | Non-zero packed offset | Zero in its own object |
| Plane modifier | Shared/unified | Plane-appropriate |
| Import complexity | Lower | Higher |
| Natural NVIDIA layout | Compromised | Preserved |
| Synchronization surface | Simpler | Must cover all objects |

Prefer per-plane export for production only if it is at least as correct and
stable as single-buffer mode. Its architectural cleanliness does not override
pixel or lifetime evidence.

If only per-plane mode flashes, the likely fault domain is plane identity,
modifier interpretation, access synchronization, or reuse. If both modes
flash identically while software decode does too, the common
VideoResourceUpdater/SharedImage/Graphite YUV sampling path remains the better
suspect.

## Phase 8: performance and power acceptance

Measure after correctness:

- CPU utilization while several animated covers are visible;
- NVIDIA decoder utilization where available;
- GPU render utilization;
- frame cadence and missed presentation opportunities;
- input-to-visible latency;
- CEF GPU-process memory;
- driver backing-image cache size;
- FD count over time.

Hardware decode is successful only if it reduces codec/upload CPU cost without
trading it for stalls, excess GPU power state, or additional frame corruption.

Evaluate `CUDA_DISABLE_PERF_BOOST=1` separately. Keep it only if it reduces
idle/video power without harming decode cadence or stability.

## Phase 9: production integration and maintenance

After the source driver and one export mode pass:

1. retain one pinned source revision;
2. add one repeatable private build/install command;
3. have Noctalia's install workflow place or reference the private artifact;
4. set the minimal environment before CEF initialization;
5. retain only the required Chromium feature gates;
6. remove temporary media and driver tracing;
7. document the rollback command and selected driver revision.

Do not create a schema, capability-negotiation protocol, or build-contract
framework for one controlled source stack. Meson, the compiler, libva,
`vainfo`, and Chromium decoder initialization already enforce the relevant
interfaces.

When updating NVIDIA, Chromium, CEF, Dawn, Skia, or the VA-API driver, rerun:

- the private-driver `vainfo` check;
- the per-plane pixel/reuse gate;
- the CEF focused tests;
- Vulkan validation;
- the live animated-art acceptance run.

## Rollback

The immediate rollback is:

1. remove the two NVIDIA VA-API CEF feature gates;
2. stop setting the private driver environment;
3. remove the versioned private prefix if desired;
4. rebuild/restart Noctalia.

Chromium then returns to software video decoding while CEF page composition
and Noctalia presentation remain Graphite/Vulkan. No profile, authentication,
or page-rendering state needs to be discarded.

## Relationship to the animated-art flicker

Hardware decode is not assumed to fix the existing intermittent yellow flash.
It is valuable because it creates a more native and observable video path and
separates likely fault domains.

Outcomes should be interpreted as follows:

| Result | Strong implication |
| --- | --- |
| Software flashes; hardware single-buffer does not | software upload/reuse path is suspect |
| Hardware single-buffer flashes; software does not | VA export/import or decoded-surface lifetime is suspect |
| Single-buffer passes; per-plane flashes | plane-specific descriptor, modifier, sync, or reuse bug |
| Both hardware modes flash with the same signature | common VA surface or Graphite YUV path is suspect |
| Software and hardware both flash | common Chromium video-quad/SharedImage/Graphite sampling path is suspect |
| The complete CEF root flashes, not only artwork | return to root-output/presentation synchronization |

The purpose of this workstream is therefore twofold:

- reduce the real CPU and upload cost of animated web video;
- establish a correct, pixel-tested decoder-to-Graphite contract that either
  resolves the artifact or sharply localizes the remaining defect.

## Final acceptance

The workstream is complete when:

- Apple Music animated artwork uses `VaapiVideoDecoder` and NVDEC on the
  compositor-selected NVIDIA GPU;
- the selected private driver revision and export mode are explicit;
- decoded video reaches Graphite/Dawn without CPU pixel readback;
- sustained animations remain correctly colored, smooth, and interactive;
- CEF close/reopen, navigation, audio, AAC, and Widevine remain functional;
- no surface, FD, CUDA allocation, or backing-image leak appears;
- all focused pixel/reuse tests pass;
- Vulkan validation remains clean at the root-frame boundary;
- temporary diagnostic code and environment are removed;
- rollback to software codec decoding remains simple and does not introduce a
  second page-rendering backend.
