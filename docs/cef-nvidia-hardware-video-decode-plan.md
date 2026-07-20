# CEF NVIDIA Hardware Video Decode Plan

Last updated: 2026-07-19

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

Use direct NV12/P010 sampling through one packed, image-wide DMA-BUF allocation.
The NVIDIA modifier reports this external-memory tuple as
`DEDICATED_ONLY | IMPORTABLE | EXPORTABLE`, so Dawn imports it as one
non-disjoint Vulkan image and chains `VkMemoryDedicatedAllocateInfo` into the
allocation. The driver exports both planes from that one allocation with
explicit offsets and pitches.

Do not retain the per-plane/disjoint experiment or the one-copy ARGB control as
production alternatives. Per-plane memory cannot satisfy NVIDIA's
dedicated-only Vulkan contract, while ARGB adds an avoidable full-frame GPU
conversion and loses P010 precision.

## Implementation status

- [x] Build the pinned `78ab5fc` driver fork from source in release mode.
- [x] Install the module into a private, versioned Noctalia directory without
  creating or replacing a system VA-API driver.
- [x] Verify the exact private artifact on `/dev/dri/renderD129` with the
  direct backend.
- [x] Verify `VAProfileH264High` with `VAEntrypointVLD`.
- [x] Scope the driver environment to Noctalia and enable Chromium's NVIDIA
  and ANGLE/Vulkan decoder gates.
- [x] Extend Chromium's Linux media selection and common accelerated-decode
  gate to recognize Vulkan-backed Graphite/Dawn.
- [x] Decouple the Graphite/Dawn/Vulkan VA-API opt-in from Chromium's
  native-Vulkan-only `GPUInfo::vulkan_info`.
- [x] Prove that Apple Music reaches `VaapiVideoDecoder`, creates an NVIDIA
  decode context, and allocates P010 decode surfaces.
- [x] Extend Dawn's Linux Vulkan DMA-BUF importer, Vulkan capability
  advertisement, and Chromium's Graphite plane test to cover P010.
- [x] Prove that Apple Music selects `VaapiVideoDecoder` and sustains NVDEC
  frame flow.
- [x] Reproduce corruption with both the old packed import and the per-plane
  disjoint experiment.
- [x] Query the exact P010/modifier/external-memory tuple on the selected
  NVIDIA device.
- [x] Prove that NVIDIA requires dedicated external memory for both packed and
  disjoint variants.
- [x] Prove that Vulkan forbids a dedicated allocation from naming a disjoint
  image.
- [x] Match Vulkan's packed plane layouts and allocation requirements against
  the driver's layout calculation at 832×1112 and 2048×2736.
- [x] Make the driver emit one packed image allocation and remove its private
  per-plane mode.
- [x] Make Dawn preserve external-memory properties, validate the packed image
  contract, and import dedicated-only memory correctly.
- [x] Restore direct NV12/P010 negotiation for Graphite/Dawn/Vulkan.
- [x] Pass the direct-P010 build, Vulkan-validation, reuse, and live
  animated-art gates with sustained hardware decode and no corruption,
  flicker, or cadence failure.
- [ ] Add a known-pattern automated NV12/P010 pixel regression to preserve the
  now-proven live contract independently of Apple Music.

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

Before this work, Noctalia enabled neither feature, so Apple Music video was
decoded in software even though Viz and Noctalia rendered on the GPU.

The first live integration run exposed one additional M151 restriction.
`GpuMojoMediaClientLinux` accepts VA-API only when
`GpuPreferences::gr_context_type` is `kVulkan`. CEF's Graphite/Dawn/Vulkan
renderer correctly reports `kGraphiteDawn`, so the media client rejects the
platform decoder before creating a VA context even though Dawn itself is
Vulkan-backed.

The maintained CEF patch stack now contains a focused correction which:

- recognizes `kGraphiteDawn` as Vulkan only when
  `--skia-graphite-dawn-backend=vulkan` is explicit;
- enables the existing NV12/P010 renderable-format preference for
  Graphite/Dawn/Vulkan as well as native Vulkan;
- does not let the absence of a legacy GL implementation disable accelerated
  decode when the active renderer is native Vulkan or Vulkan-backed
  Graphite/Dawn;
- leaves Graphite/Dawn on every other backend, Ganesh GL, and ordinary
  Chromium behavior unchanged.

The common GL consistency check was a separate, earlier filter from the Linux
decoder implementation selector. The first driver-level run made that ordering
visible: VA-API and NVDEC loaded for capability enumeration, then the VA display
terminated without an H.264 decoder instance or decode surface ever being
created. This is required in addition to the runtime feature gates. Driver
initialization or capability enumeration alone does not prove the platform
decoder survived either media-client selection step.

The first animated-art acceptance run then exposed a second backend-identity
assumption in the same selector. Chromium's native Vulkan implementation
populates `GPUInfo::vulkan_info`; Dawn's independently owned Vulkan instance
does not. Reclassifying Graphite/Dawn/Vulkan as a Vulkan renderer therefore
reached a native-Vulkan metadata check that it could never pass.

The maintained patch now handles that distinction explicitly:

- native Vulkan retains Chromium's existing `vulkan_info`, vendor, and driver
  version checks;
- Graphite/Dawn/Vulkan is accepted only for the selected NVIDIA GPU and only
  when both `VaapiOnNvidiaGPUs` and `VaapiIgnoreDriverChecks` are explicitly
  enabled;
- every other Graphite/Dawn backend and unapproved vendor remains rejected.

This is narrower than synthesizing native Vulkan metadata or bypassing the
decoder checks globally.

The 2026-07-18 animated-art acceptance run proved that this selection work
reaches the real decoder. The NVIDIA VA-API driver created an 832×1112 decode
context and nineteen 10-bit decode surfaces. Each surface used one allocation
with two explicit layouts and was exported as `DRM_FORMAT_P010`.

That run then stopped at two consecutive Dawn format-support gaps. First,
Dawn's Linux Vulkan DMA-BUF importer recognized NV12 but omitted
`DRM_FORMAT_P010`, so the imported format never reached Graphite. Adding the
mapping advanced the next acceptance run to Dawn device creation, where the
same backend rejected P010 because its Vulkan adapter never advertised
`MultiPlanarFormatP010`.

The second failure is also a Dawn Vulkan omission rather than a capability
failure on the selected GPU. Dawn's D3D path exposes P010, but its Vulkan
feature probe only checked NV12. The maintained Dawn-owned nested patch now:

- maps `DRM_FORMAT_P010` to `R10X6BG10X6Biplanar420Unorm`;
- advertises `MultiPlanarFormatP010` only when the physical device supports
  sampled, linearly filtered, and transfer-capable P010 images plus filterable
  R16/RG16 plane views;
- leaves GPUs without that complete contract unsupported.

Chromium requests Dawn's restricted
`Unorm16FormatsForExternalTexture` capability when available, and the existing
Graphite/Dawn multiplanar regression test asserts P010's `R16Unorm` Y view and
`RG16Unorm` UV view.

The next live build passed both Dawn capability gates and sustained real NVDEC
work at 2–11% decoder utilization while Vulkan synchronization validation
remained clean. It did not pass pixel correctness: animated P010 artwork
intermittently alternated among green frames and heavily tiled red/green
corruption while the surrounding page remained correct. This localizes the
remaining failure before the final ARGB CEF-to-Noctalia bridge, at the
decoder-surface-to-Viz multiplanar import boundary.

The subsequent disjoint experiment preserved each NativePixmap plane FD,
created the Vulkan image with `VK_IMAGE_CREATE_DISJOINT_BIT`, and bound one
allocation per memory plane. It still corrupted real P010 frames at 832×1112
and 2048×2736.

The decisive result came from querying Vulkan rather than inferring support
from successful object creation. For the exact NVIDIA modifier
`0x0300000000606014`, `vkGetPhysicalDeviceImageFormatProperties2` reports
external-memory features `0x7`: dedicated-only, exportable, and importable.
Vulkan requires a dedicated-only import to chain
`VkMemoryDedicatedAllocateInfo` naming the image, but that structure is
forbidden from naming an image created with `VK_IMAGE_CREATE_DISJOINT_BIT`.
The disjoint topology therefore cannot be valid on this device.

The original packed attempt also omitted
`VkMemoryDedicatedAllocateInfo`. Vulkan validation did not report it because
the dedicated-only requirement is returned by a capability query and the
validation layer does not connect that earlier semantic result to a later
allocation. The packed layout itself is not the flaw: raw Vulkan image queries
return the same Y/UV offsets, pitches, and allocation sizes that the driver
calculates for both observed Apple Music dimensions.

The production correction coordinates both sides:

- the driver allocates one image-wide block-linear object, maps both CUDA plane
  arrays at their explicit offsets, imports the packed allocation into CUDA
  without the single-image dedicated flag, and emits one DMA-BUF object in the
  VA descriptor;
- Dawn confirms that all plane FDs refer to the same object, checks the
  modifier's returned plane layouts, checks the DMA-BUF size against Vulkan's
  image requirement, and chains `VkMemoryDedicatedAllocateInfo` whenever the
  external-memory query requires it;
- Graphite/Dawn/Vulkan again advertises NV12/P010 as preferred renderable
  formats, so Chromium does not insert the ARGB VideoProcessor conversion.

### Driver status

Ubuntu's `nvidia-vaapi-driver` package is version `0.0.14`. It exposes the
RTX 4070 Ti's H.264, HEVC, VP8, VP9, and AV1 decode profiles successfully, but
it predates the recent Chromium-oriented direct-backend work.

The private fork and pinned revision are:

```text
https://github.com/maddythewisp/nvidia-vaapi-driver.git
78ab5fc
```

The fork is based on upstream `03bb5a0` (`v0.0.17-54-g03bb5a0`). Relative to
Ubuntu's `v0.0.14`, it contains work directly relevant to this integration:

- Chromium stream-format transition handling;
- waiting for display-surface resolution before export;
- backing-image reuse and bounded detached-image caching;
- reduced per-frame host synchronization and allocation;
- packed direct-backend export with explicit plane layouts;
- plane-specific block-height and modifier fixes;
- chroma-corruption fixes;
- cleanup of failure, rollback, and resolver-thread lifetimes.

The apt package has been removed. The pinned source build is installed only
under the driver's package-owned, user-private versioned prefix:

```text
~/.local/lib/nvidia-vaapi-driver/78ab5fc/lib/dri/nvidia_drv_video.so
~/.local/lib/nvidia-vaapi-driver/current -> 78ab5fc
```

No system VA-API driver file is created or replaced.

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
  -> decoded NV12/P010 DMA-BUF SharedImage
  -> Chromium VideoResourceUpdater / TextureDrawQuad
  -> Viz Skia Graphite / Dawn Vulkan
  -> complete ARGB CEF root frame
  -> CEF offscreen DMA-BUF output queue
  -> Noctalia Vulkan import
  -> Noctalia Graphite sampling
  -> Wayland presentation
```

The first boundary transports a decoded video frame as multiplanar NV12 or
P010. The second transports the already-composited CEF root frame as ARGB.

The animated-art yellow flash may originate at the first boundary and then be
faithfully captured in the second. Hardening only `CefGpuFrameBridge` cannot
repair a video frame that was already sampled with stale or misinterpreted
chroma inside Viz.

## Semiplanar export models

NV12 and P010 both contain:

- a full-resolution Y plane carrying brightness;
- a half-resolution interleaved UV plane carrying color.

NV12 stores 8-bit components. P010 stores each 10-bit component in a 16-bit
word, so Graphite/Dawn must expose its planes as `R16Unorm` and `RG16Unorm`
views of the multiplanar P010 texture rather than reinterpreting them as
8-bit NV12.

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

### Per-plane export — rejected

```text
DMA-BUF object 0: Y
DMA-BUF object 1: UV
```

This topology was useful diagnostically, but is not valid for the observed
NVIDIA external-memory contract. A dedicated-only allocation must name the
complete image, while Vulkan forbids that dedicated allocation from naming a
disjoint image. The maintained driver patch therefore removes the per-plane
mode and its `NVD_SINGLE_BUFFER` selector rather than retaining two layouts.

## Required invariants

Every hardware-decoded frame must satisfy all of the following:

1. NVDEC, Chromium's GPU media client, Dawn, Viz, and Noctalia use the
   compositor-selected RTX 4070 Ti.
2. The VA-API surface is not exported until decode or VideoProc resolution
   has completed.
3. The export contains exactly one DMA-BUF object for the complete image; its
   plane ordering, strides, offsets, sizes, and modifier describe that object.
4. Y and UV belong to the same decoded content frame.
5. The acquire dependency makes all decoder writes to both planes visible
   before Dawn/Graphite sampling.
6. Chromium cannot recycle the image while Viz may still sample either plane.
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

Build, privately publish, and verify the pinned fork:

```sh
cd ~/Developer/nvidia-vaapi-driver
./deploy-private.sh --render-device /dev/dri/renderD129
```

The expected driver directory is:

```text
~/.local/lib/nvidia-vaapi-driver/d9de2ad/lib/dri
```

The script uses the driver's ordinary Meson release build, publishes the
completed module under a revision-named private directory, atomically updates
`current`, and runs `vainfo` against that exact artifact. It never creates or
replaces a system VA-API driver.

Installed Noctalia resolves the same `current/lib/dri` path relative to its own
installation prefix. Build-tree runs fall back to the per-user deployment
above. `NOCTALIA_NVIDIA_VAAPI_DRIVER_PATH` may override the driver directory
for explicit development or rollback testing; it is not a required runtime
setting.

Verify that exact artifact against CEF's render node:

```sh
LIBVA_DRIVER_NAME=nvidia \
LIBVA_DRIVERS_PATH="$HOME/.local/lib/nvidia-vaapi-driver/current/lib/dri" \
NVD_BACKEND=direct \
NVD_DRM_DEVICE=/dev/dri/renderD129 \
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

Add these NVIDIA-specific CEF features:

```text
VaapiOnNvidiaGPUs
VaapiIgnoreDriverChecks
```

Chromium's Vulkan media client also requires these ANGLE/Vulkan interop
features:

```text
DefaultANGLEVulkan
VulkanFromANGLE
```

`--use-angle=vulkan` selects ANGLE's backend but does not enable either
feature. Both are disabled by default in Chromium M151, and
`GetActualPlatformDecoderImplementation()` rejects VA-API on the Vulkan path
unless both feature states are enabled.

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

Chromium M151 also contains a common accelerated-decoder consistency check
that treats `gl::GetGLImplementation() == kGLImplementationDisabled` as a
decoder-disable signal. That assumption is valid for its historical GL path
but not for an explicitly Vulkan-backed Graphite/Dawn context. Keep the
correction backend-specific: bypass the GL consistency check only for native
Vulkan or `kGraphiteDawn` with
`--skia-graphite-dawn-backend=vulkan`.

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

## Phase 5: enforce the packed Vulkan-import contract

The driver exports every NV12/P010 surface as one DMA-BUF object with explicit
Y and UV offsets and pitches. Dawn imports that one object into one
non-disjoint Vulkan image and chains `VkMemoryDedicatedAllocateInfo` whenever
the exact external-image-format query reports
`VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT`.

Test:

- both observed Apple Music sizes, 832×1112 and 2048×2736;
- small NV12/P010 heights that exercise block-height selection;
- multiple animated album and playlist covers;
- navigation between static and animated artwork;
- repeated HLS loop boundaries;
- panel close/reopen;
- page reload and browser recreation;
- panel resize and fractional scaling;
- simultaneous audio playback and artwork animation;
- Apple Music login/profile persistence;
- AAC playback and Widevine playback.

This phase is complete when:

- hardware decode remains selected;
- the VA descriptor contains one object and correct packed plane layouts;
- Dawn reports no layout, allocation-size, or memory-type rejection;
- direct P010 pixels are correct;
- Vulkan synchronization validation remains clean.

The live gate passed on the compositor-selected RTX 4070 Ti. CUDA decode,
CUDA plane mapping, DRM export, Dawn import, Graphite composition, and
Noctalia presentation all remained on `/dev/dri/renderD129`; animated Apple
Music artwork remained continuous with no corruption or flicker.

## Phase 6: establish a real direct-P010 correctness gate

Extend the Chromium/CEF test coverage to create known NV12 and P010 frames in
one packed DMA-BUF object and render them through the same
SharedImage → Dawn → Graphite path used by video.

The test must cover:

- known luma and chroma patterns with exact expected RGBA pixels;
- both plane texture-view aspects;
- different legal plane strides and sizes;
- packed plane offsets, strides, modifier, and allocation size;
- full-range and limited-range video;
- the color spaces used by Apple artwork;
- repeated frame reuse under producer/consumer fences;
- rapid alternation of chroma values so stale UV is unmistakable;
- resolution and stream-format changes;
- retirement while frames remain in flight;
- allocation/import failure rollback.

A static “the descriptors look right” unit test is insufficient. The gate
must read back rendered pixels and exercise reuse.

Audit the Linux Graphite/Dawn path for formats marked
`PrefersExternalSampler()`. That patch deliberately replaces the stock
external-sampler behavior with explicit plane images on Linux. Confirm that:

- the promise texture metadata accepts the fulfilled plane aspect;
- the Y and UV views reference the same image allocation;
- begin/end access covers the complete image;
- one frame-level dependency covers both planes;
- release occurs after the last Graphite use;
- cached representations cannot combine content from different frames.

## Phase 7: complete live direct-P010 acceptance

Graphite/Dawn/Vulkan advertises NV12 and P010 as directly renderable. Chromium
therefore hands the decoded semiplanar image to its SharedImage/Graphite path
without inserting the ARGB VideoProcessor conversion. Repeat the complete live
acceptance matrix on that path.

The accepted production flow is:

```text
compressed AVC
  -> NVDEC P010 decode surface
  -> packed P010 NativePixmap / SharedImage
  -> R16 Y and RG16 UV Graphite plane views
  -> Graphite/Dawn/Vulkan composition
```

This preserves the decoder's 10-bit surface, performs no CPU pixel readback,
and avoids a full-frame P010-to-ARGB conversion.

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

After the source driver and direct-P010 path pass:

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
- the packed direct-P010 pixel/reuse gate;
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
| Software flashes; packed dedicated P010 does not | software upload/reuse path is suspect |
| Packed P010 import rejects its contract | inspect the exact modifier, layout, size, and dedicated-memory query |
| Packed P010 imports but corrupts | inspect CUDA's plane mapping and Graphite's plane views with the pixel gate |
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
