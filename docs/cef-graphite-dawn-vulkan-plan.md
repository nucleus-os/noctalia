# CEF Graphite/Dawn/Vulkan Migration Plan

Last updated: 2026-07-17

## Decision

Migrate CEF's Viz renderer from Skia Ganesh over ANGLE Vulkan to Skia Graphite
over Dawn Vulkan while preserving Noctalia's direct DMA-BUF sampling path.

This is a hard renderer migration, not a runtime-selectable experiment. The
finished integration will not retain a Ganesh, CPU-paint, copied-frame, or
disabled-CEF fallback.

ANGLE Vulkan remains enabled for WebGL and other GL-originated Chromium
content. Graphite/Dawn replaces Viz's Skia renderer; it does not eliminate
every internal GL producer.

CEF and Noctalia continue to own separate Vulkan devices and queues. The
DMA-BUF ownership and synchronization boundary therefore remains part of the
architecture and must be exact.

The authoritative description of the currently deployed frame path remains
`docs/cef-zero-copy-frame-architecture.md`. Update that document when this
migration lands.

## Implementation status

- [x] Phase 1: preserve Dawn's exact external Vulkan layout state for the
  Ozone offscreen-export round trip.
- [x] Phase 2: transport producer old/new layouts through Mojo and CEF.
- [x] Phase 3: split Noctalia's ownership and local-layout barriers, normalize
  releases to `GENERAL`, and remove access state from import identity.
- [x] Phase 4: route the exportable offscreen endpoint exclusively through
  Graphite/Dawn/Vulkan.
- [x] Phase 5: select Dawn's Vulkan adapter by the requested DRM render node.
- [x] Phase 6: make Graphite/Dawn/Vulkan mandatory while retaining Vulkan
  ANGLE for GL-originated content.
- [x] Phase 7: consolidate the work into patches 0001, 0002, and 0004 without
  adding an overlapping patch.
- [x] Phase 8: finish patch-application, generated-project, source, and test
  checks.
- [ ] Phase 9: build, package, install, and perform runtime acceptance.

### Pre-build verification completed

The source stack is ready for the first CEF build:

- patches 0001, 0002, and 0004 apply cleanly to their exact pinned M151
  baselines;
- the complete seven-patch stack has no file overlap between patches;
- all newly added implementation and unit-test sources are present in the
  generated project;
- CEF's translator and API-hash regeneration complete successfully;
- GN generation completes successfully with 31,835 targets;
- the focused test sources resolve into
  `//components/viz/service:unit_tests`,
  `//cef:libcef_static_unittests`, and `//gpu:gl_tests`;
- Noctalia and Nucleus repository diffs pass `git diff --check`;
- the Chromium/CEF patches contain no Noctalia-specific naming.

The tests have not been compiled or executed yet. That work belongs to Phase
9 so it uses the exact optimized CEF build that will be packaged and tested at
runtime.

## Motivation

Intermittent yellow flashes remain visible in continuously animated Apple
Music album and playlist artwork after hardening Noctalia's Graphite sampling,
buffer lifetime, release fencing, and presentation scheduling.

The failure's concentration in continuously changing artwork leaves
Chromium's Ganesh/ANGLE SharedImage path as an important remaining suspect.
Graphite/Dawn replaces that producer-side rendering path and moves CEF closer
to Noctalia's Graphite/Vulkan architecture.

This migration is not expected to fix the artifact merely because both sides
use Graphite. The two processes do not share a Graphite context. Its value is
that it replaces Ganesh/ANGLE inside Chromium and gives the external handoff a
Vulkan-native producer.

## Critical prerequisite: preserve Dawn's Vulkan layout state

Enabling Graphite alone is unsafe.

The offscreen export round trip described below is implemented. Chromium's
broader Ozone layout-tracking TODO remains: initialized internal backings can
arrive at Dawn through access paths that do not yet record a transfer. The
shipping compatibility behavior uses exact state when present and retains
upstream's first/discard transition otherwise, while exported frames remain
strict. The full replacement is specified in
`chromium-ozone-vulkan-access-state-plan.md`.

Dawn's Vulkan `SharedTextureMemory::EndAccess()` returns the exact
`oldLayout`/`newLayout` pair used by its release to
`VK_QUEUE_FAMILY_EXTERNAL_KHR`. Chromium's
`DawnOzoneImageRepresentation` currently discards that pair. Its next
`BeginAccess()` supplies `VK_IMAGE_LAYOUT_UNDEFINED` for both values under an
upstream layout-tracking TODO.

The existing CEF transport also exposes only one hardcoded
`VK_IMAGE_LAYOUT_GENERAL` value. That is insufficient to match an arbitrary
Dawn ownership release.

The Graphite/Dawn migration must therefore implement the complete layout
round trip before switching the renderer:

1. Dawn reports its exact external-release layout pair.
2. The producer stores the pair alongside the acquire fence.
3. CEF transports the pair with the matching frame.
4. Noctalia uses the exact pair for its ownership acquire.
5. Noctalia transitions to shader-read layout separately.
6. Noctalia normalizes the image to `GENERAL`.
7. Noctalia releases ownership with a `GENERAL` to `GENERAL` barrier.
8. CEF/Dawn begins its next access from `GENERAL` to `GENERAL`.

Vulkan validation cannot compare barriers recorded in separate processes.
This must remain an explicit, tested cross-process invariant even when both
validation layers report no errors.

## Target architecture

```text
Blink / media / raster
        |
        v
Chromium Viz SkiaRenderer
        |
        v
Skia Graphite
        |
        v
Dawn Vulkan on the compositor-selected GPU
        |
        v
Four-slot DMA-BUF output pool
        |
        | sync_fd + exact layout pair + EXTERNAL ownership
        v
Noctalia Vulkan ownership acquire
        |
        v
Noctalia Graphite sampling
        |
        v
Noctalia normalizes to GENERAL and releases to EXTERNAL
        |
        | sync_fd
        v
CEF/Dawn reacquires the slot
```

## Required invariants

Every frame must satisfy all of the following:

1. Dawn and Noctalia select the compositor-presenting physical GPU.
2. Dawn completes rendering before exporting the producer acquire fence.
3. The producer reports the exact Vulkan ownership-release
   `(oldLayout, newLayout)` pair.
4. Noctalia's ownership-acquire barrier uses that exact pair.
5. Noctalia transitions to shader-read layout only after ownership
   acquisition.
6. The Graphite recording that samples the image waits on the exact acquire
   dependency.
7. Noctalia retains the image until every Graphite submission that sampled it
   has completed.
8. Noctalia transitions the image to `GENERAL` before releasing ownership.
9. Noctalia's ownership-release barrier is `GENERAL` to `GENERAL`, from its
   graphics queue to `VK_QUEUE_FAMILY_EXTERNAL_KHR`.
10. CEF waits on Noctalia's release fence and gives Dawn the matching
    `GENERAL` to `GENERAL` acquire state.
11. Viz cannot render into a slot again until the consumer release completes.
12. A frame superseded before sampling still receives a balanced acquire and
    release.
13. No fallback may silently change CEF back to Ganesh, software paint, or a
    copied image path.

## Implementation phase 1: track external Vulkan state in Chromium

Add a small generic value type, conceptually:

```cpp
struct ExternalVulkanImageState {
  VkImageLayout old_layout;
  VkImageLayout new_layout;
};
```

The type describes the image-layout pair used by an external queue-family
ownership transfer. It is access state, not buffer identity.

Update the Linux Ozone/Dawn path as follows:

- `DawnOzoneImageRepresentation::EndAccess()` stores the layout pair returned
  by `SharedTextureMemory::EndAccess()` in `OzoneImageBacking`.
- `DawnOzoneImageRepresentation::BeginAccess()` reads the state stored by the
  previous external owner instead of using `UNDEFINED`/`UNDEFINED`.
- The overlay representation makes the layout state associated with its
  acquire fence available to the offscreen export device.
- Ending overlay access after Noctalia's release stores
  `GENERAL`/`GENERAL` as the state for Dawn's next access.
- Fence and layout state move as one logical handoff. State from one frame or
  slot must never be associated with another.

Keep the additions generic to Chromium external offscreen consumption. Do not
put Noctalia-specific terminology in the Chromium patch.

### Phase 1 tests

Add focused tests covering:

- Dawn write to overlay read transfers the returned layout pair;
- overlay release to Dawn write supplies `GENERAL`/`GENERAL`;
- a missing or invalid layout state prevents offscreen frame publication;
- an uncleared first-use internal image may take the explicit discard path,
  while initialized preservation paths are covered by the long-term Ozone
  access-state plan;
- layout state remains attached to the correct backing and access;
- resize and retired-slot destruction do not leak stale state.

## Implementation phase 2: extend the offscreen frame transport

Replace the singular producer layout field with:

```text
producer_old_layout
producer_new_layout
queue_family_index
```

Carry these fields through:

1. `mojom::OffscreenOutputFrame`;
2. Viz-to-CEF Mojo transport;
3. CEF accelerated-paint metadata;
4. `BorrowedDmabufFrame`;
5. `CefGpuFrameBridge::PendingFrame`.

The queue-family value remains
`VK_QUEUE_FAMILY_EXTERNAL_KHR`.

No release-layout fields are required in the return call. The consumer
contract requires Noctalia to normalize every returned image to `GENERAL` and
use `GENERAL`/`GENERAL` for the ownership release.

Retain the existing frame identity:

- transport epoch;
- capture counter;
- output generation;
- output slot;
- content serial.

Do not add schema negotiation, runtime API probing, compatibility modes, or
capability-version machinery. CEF and Noctalia are built and shipped as one
known source stack.

## Implementation phase 3: correct Noctalia's Vulkan barriers

### Producer to Noctalia

Record two barriers.

The first barrier performs only the matching ownership acquire:

```text
oldLayout          = producer_old_layout
newLayout          = producer_new_layout
srcQueueFamily     = EXTERNAL
dstQueueFamily     = Noctalia graphics queue
```

The second barrier is local to Noctalia:

```text
oldLayout          = producer_new_layout
newLayout          = SHADER_READ_ONLY_OPTIMAL
srcQueueFamily     = IGNORED
dstQueueFamily     = IGNORED
```

Signal acquire-complete after both barriers. Attach that semaphore to the
exact Graphite recording that samples the imported image.

### Noctalia to producer

Record two release-side barriers.

First normalize the local layout:

```text
oldLayout          = SHADER_READ_ONLY_OPTIMAL
newLayout          = GENERAL
srcQueueFamily     = IGNORED
dstQueueFamily     = IGNORED
```

Then release ownership:

```text
oldLayout          = GENERAL
newLayout          = GENERAL
srcQueueFamily     = Noctalia graphics queue
dstQueueFamily     = EXTERNAL
```

Export the completion semaphore as a sync FD and return it through the
token-correlated CEF release callback.

### Import-cache correction

Remove queue-family and image-layout values from `ImportKey`.

The imported Vulkan image is identified by:

- transport epoch;
- stable file device and inode;
- dimensions;
- format;
- modifier;
- plane stride and offset.

The layout pair belongs to `PendingFrame`, because it can change on every
access without changing the DMA-BUF allocation. Treating it as cache identity
could create duplicate imports or recurring evictions for the same producer
slot.

## Implementation phase 4: route Graphite/Dawn through the export device

`SkiaOutputDeviceOffscreenExport` is already constructed with either a Ganesh
context or Graphite shared context, and Chromium's output-device base class can
insert Graphite recordings into a target `SkSurface`.

Extend `SkiaOutputSurfaceImplOnGpu::InitializeForDawn()` so an external
offscreen output connection selects `SkiaOutputDeviceOffscreenExport`.

The output device must require:

- `context_state_->IsGraphiteDawnVulkan()`;
- top-left origin;
- premultiplied alpha;
- a Graphite Skia representation;
- an overlay/native-pixmap representation;
- an exportable acquire fence;
- valid external Vulkan layout state.

The SharedImage usage remains display read, display write, and scanout. The
selected backing must support both Graphite write access and DMA-BUF/overlay
export.

If the SharedImage factory cannot create that combination, fail the output
endpoint with a named error. Do not copy through another image and do not
select the ordinary offscreen output device.

### Phase 4 tests

Prove that the selected SharedImage supports:

- Graphite write access;
- overlay read access;
- one-plane BGRA or RGBA DMA-BUF export;
- a producer fence covering Graphite/Dawn completion;
- the exact external layout pair;
- consumer-fence return before slot reuse.

## Implementation phase 5: pin Dawn to the compositor GPU

The existing CEF patch pins Chromium native Vulkan and ANGLE, but Dawn
currently enumerates adapters and selects the first one.

Extend the existing device-selection patch to select Dawn using the current
`--render-node-override` value:

1. `stat()` the requested DRM render node.
2. Query each Vulkan Dawn adapter with `wgpu::AdapterPropertiesDrm`.
3. Compare the adapter's render major/minor with the requested node.
4. Select the matching adapter.
5. Fail Dawn initialization if no adapter matches.

Render-node identity is preferable to vendor/device IDs because it
distinguishes multiple GPUs from the same vendor.

Emit one concise startup message containing:

```text
Skia backend: Graphite/Dawn/Vulkan
Dawn adapter: <device name>
DRM render node: <path>
```

## Implementation phase 6: make Graphite/Dawn mandatory

Build CEF with only Chromium's Ozone Wayland backend:

```text
ozone_platform=wayland
ozone_platform_wayland=true
ozone_platform_x11=false
```

This removes the X11 Ozone backend and fallback from the production target.
CEF remains an offscreen producer: Noctalia owns the visible `wl_surface`,
while CEF uses Ozone Wayland for Linux native-pixmap and DMA-BUF integration.

Update `src/cef/noctalia_cef_app.cpp` to request:

```text
--enable-skia-graphite
--skia-graphite-dawn-backend=vulkan
```

Keep:

```text
--ozone-platform=wayland
--use-angle=vulkan
--vulkan-device-uuid=...
--render-node-override=...
```

`--use-angle=vulkan` remains necessary for WebGL and other GL-originated
content. It no longer selects Viz's Skia renderer.

After the Graphite export path is proven:

- route the accelerated offscreen output connection only through
  `InitializeForDawn()`;
- remove its custom Ganesh GL and native-Vulkan output-device selection;
- keep `OnPaint` unsupported;
- surface a renderer error if Graphite/Dawn initialization fails.

If Chromium silently falls back to Ganesh, the custom accelerated endpoint
must not remain available. Failure is preferable to unknowingly testing the
wrong backend.

## Implementation phase 7: keep the patch stack consolidated

Modify the existing patches rather than adding overlapping patches:

- `0001-viz-offscreen-output-transport.patch`
  - Ozone/Dawn layout-state handling;
  - Graphite/Dawn output-device routing;
  - Mojo layout fields;
  - output/backing tests.

- `0002-cef-osr-dmabuf-and-frame-scheduling.patch`
  - CEF accelerated-paint layout fields;
  - propagation through the OSR callback.

- `0004-pin-vulkan-device.patch`
  - Dawn adapter selection by DRM render node.

Do not create a later patch that modifies the same source areas. Keep
Chromium patches generic and free of Noctalia-specific naming.

## Implementation phase 8: finish all source work before building

Complete the following before starting the expensive CEF build:

- Chromium/Viz/Dawn/Ozone changes;
- CEF API and wrapper changes;
- Noctalia bridge changes;
- unit-test source;
- regenerated patch files;
- clean patch application against the pinned M151 source;
- GN generation;
- review of the final patch stack.

Cache and generated source directories remain generated outputs and must not
be edited as authoritative source.

## Implementation phase 9: build, package, and install

1. Build the optimized CEF configuration with the existing official-build,
   PGO, and ThinLTO setup.
2. Run the focused Viz/Ozone tests from that build.
3. Package `libcef`, headers, resources, and `libcef_dll_wrapper` from the same
   build to preserve the API hash.
4. Reconfigure and rebuild Noctalia against the new accelerated-paint
   metadata.
5. Install CEF and Noctalia together.
6. Replace the running shell.

The Nucleus render SDK does not need to be rebuilt merely to enable CEF
Graphite/Dawn. CEF already builds Dawn and its Vulkan backend; Noctalia
continues using Nucleus's native-Vulkan Graphite context.

## Validation and acceptance

### Backend identity

Require positive evidence that CEF is using:

```text
GraphiteDawnVulkan
```

Verify that Dawn's DRM render node matches Noctalia's selected Vulkan device.
Do not infer success merely from the absence of a startup error.

### Transport correctness

For every accelerated frame, require:

- one valid DMA-BUF;
- one valid producer acquire fence;
- exact producer old/new layouts;
- external queue ownership;
- monotonic frame identity;
- one eventual release fence;
- no producer slot reuse before release.

### Validation layers

Enable:

- Noctalia Vulkan validation;
- Vulkan synchronization validation;
- Dawn Vulkan backend validation.

Cross-process ownership matching remains an explicit acceptance criterion
because neither validation instance can observe the other process's barrier.

### Functional acceptance

Test:

- Apple Music's transparent root;
- corrected CSS backdrop blur;
- sidebar and playback-bar styling;
- authentication persistence;
- AAC playback and normal audio control;
- navigation and text input;
- mouse and keyboard back/forward;
- draggable overlay scrollbars;
- panel close/reopen without navigation reset;
- resize and fractional scaling;
- repeated hidden/shown cycles;
- suspend/resume where practical.

### Animated-art acceptance

Exercise several continuously animated playlist and album artworks for at
least 10 to 15 minutes.

Acceptance requires:

- no yellow flashes;
- no stale artwork;
- no tearing or partial-frame corruption;
- continuous animation without mouse interaction;
- no corresponding interaction stalls.

### Resource acceptance

Require:

- the producer output pool remains bounded to four slots;
- the Noctalia import cache stabilizes;
- FD counts stabilize;
- no steady-state per-frame Vulkan image allocation;
- no steady-state CPU fence wait;
- no CEF CPU paint.

### Performance acceptance

Measure performance only after correctness is established and validation is
disabled.

Graphite persistent shader caching and precompilation may be evaluated after
the correctness build if shader warm-up causes hitches. Do not mix those
changes into the first Graphite/Dawn runtime evaluation.

## Stop conditions and interpretation

If Graphite/Dawn cannot create an exportable SharedImage backing, fix the
Graphite/Ozone backing integration. Do not restore Ganesh or add a copy
fallback.

If validation or explicit state checks reveal a layout or synchronization
error, stop functional testing and repair the transport contract.

If the yellow flashes disappear, the likely cause was the Ganesh/ANGLE
producer path or its interaction with external SharedImages.

If the flashes remain and a producer-side capture already contains the yellow
frame, the fault is inside Chromium's media, raster, Viz, or page rendering.
A narrowly scoped producer capture is then the next diagnostic.

If the producer frame is correct while Noctalia presents a flash, the
remaining fault is conclusively in the external handoff or consumer sampling
path.

## Recommended execution order

Implement the phases in this order:

1. preserve Dawn layout state;
2. extend the frame transport;
3. correct Noctalia's acquire/release barriers and cache identity;
4. route Dawn through the export output device;
5. pin the Dawn adapter;
6. make Graphite/Dawn mandatory;
7. consolidate and review the patch stack;
8. complete all source and test work;
9. perform one CEF build, one Noctalia rebuild, and the runtime acceptance
   sequence.

Do not enable Graphite before phases 1 through 3 are complete. The current
discarded Dawn layout state and hardcoded transport layout do not form a safe
zero-copy contract.
