# CEF Partial-Damage Offscreen Output Plan

Last updated: 2026-07-18

## Status

Design complete; implementation has not started.

- [ ] Phase 0: preserve the current correctness baseline.
- [ ] Phase 1: move root allocation and buffer-age tracking to Chromium's
  renderer-owned `BufferQueue`.
- [ ] Phase 2: enable partial swap and publish true root damage.
- [ ] Phase 3: finish lifecycle, generation, and backpressure behavior.
- [ ] Phase 4: pass rotating-buffer content-preservation tests.
- [ ] Phase 5: pass CEF/Noctalia live correctness acceptance.
- [ ] Phase 6: demonstrate a release-build performance improvement.
- [ ] Phase 7: remove obsolete code and update the architecture documents.

## Decision

Replace CEF's current full-root offscreen export with Chromium's native
partial-swap and renderer-owned `BufferQueue` model.

The implementation must continue exporting complete, independently valid
DMA-BUF frames. Partial damage changes how Viz updates a rotating buffer; it
does not permit Noctalia to receive a buffer whose unchanged pixels are stale
or uninitialized.

Do not add a second damage-history implementation based on CEF capture
counters, file descriptors, or inferred buffer age. Chromium already has the
required distinction between:

- **root damage**: pixels changed by the current web frame relative to the
  previously published frame; and
- **framebuffer damage**: all pixels changed while the particular rotating
  render buffer was not current.

`DirectRenderer::ComputeScissorRectForRenderPass()` unions those rectangles
and expands them for pixel-moving filters. `BufferQueue` accumulates
framebuffer damage independently for every buffer. The CEF endpoint should
reuse that machinery by becoming a surfaceless presentation/export sink for
renderer-owned root buffers.

This is a producer-side optimization. The first implementation will not add
partial redraw to Noctalia's Vulkan swapchains. Noctalia still composites one
complete CEF texture whenever CEF publishes a changed frame.

## Current state and cost

The deployed endpoint is `SkiaOutputDeviceOffscreenExport`. It allocates and
rotates four root render targets itself.

Its current capabilities leave `supports_post_sub_buffer` disabled. This
causes:

```text
SkiaRenderer::CanPartialSwap() == false
  -> DirectRenderer::use_partial_swap_ == false
  -> every non-skipped root draw becomes full-surface damage
  -> Viz records and renders the complete Apple Music page
  -> the exporter reports gfx::Rect(size_) as frame damage
```

The output device also ignores `Present(update_rect)` and unconditionally
publishes:

```cpp
exported->damage_rect = gfx::Rect(size_);
```

At the normal 1.5 fractional scale, a 1092x692 logical browser is roughly
1638x1038 physical pixels. One BGRA root buffer is about 6.5 MiB. At 60 changed
frames per second, full-root output accounts for roughly 390 MiB/s of target
writes before including intermediate render passes, filters, texture reads,
and compositing work.

Raw memory bandwidth is unlikely to be the only or dominant cost on the target
GPU. The larger opportunity is avoiding full-page Viz traversal, raster,
Graphite/Dawn work, and fragment processing when only an animated cover,
playback control, cursor effect, or small text region changed.

The transport already carries a damage rectangle from Viz through Mojo and
CEF as `capture_update_rect`. The missing work is producing a truthful
partial rectangle while keeping every rotating buffer complete.

## Why enabling one capability is incorrect

The endpoint has four buffers because a published buffer remains owned by
Noctalia until its Graphite sampling completes and its release fence reaches
Viz. Consecutive frames generally render into different buffers.

Assume a page changes through damage regions `A`, `B`, and `C`:

```text
global content: F0 -> F1(A) -> F2(B) -> F3(C)
slot 0 content: F0
slot 1 content: F1
slot 2 content: F2
```

When slot 0 returns, drawing only `C` into it would produce:

```text
slot 0 = F0 + C
```

It would still be missing changes `A` and `B`. Publishing that slot would
violate the complete-frame contract and produce stale regions or flashes.

The correct update for slot 0 is:

```text
current root damage       = C
slot-0 framebuffer damage = A union B
actual slot-0 redraw      = A union B union C
published frame damage    = C
```

The actual redraw repairs the selected buffer. The published damage remains
`C`, because that is the difference between the last globally published frame
and this frame. Chromium's existing partial-swap design already models this
separation.

## Target architecture

```text
Blink / compositor frame
        |
        v
SurfaceAggregator root damage
        |
        v
DirectRenderer
  root damage
    union
  BufferQueue::CurrentBufferDamage()
    union
  pixel-moving-filter expansion
        |
        v
Skia Graphite / Dawn Vulkan renders into renderer-owned root SharedImage
        |
        v
root SharedImage scheduled as the single offscreen primary plane
        |
        v
SkiaOutputDeviceOffscreenExport
  acting as a surfaceless export/presentation sink
  - exports DMA-BUF + acquire fence + exact Vulkan layout pair
  - publishes true root damage
  - retains the buffer until Noctalia's release fence
        |
        v
CEF OnAcceleratedPaint
        |
        v
Noctalia direct Graphite sampling
```

The renderer-owned `BufferQueue` is the authority for:

- buffer allocation and rotation;
- full damage on first use and reshape;
- accumulated per-buffer framebuffer damage;
- buffer availability after swap completion;
- keeping at least one renderable buffer outside the pending-swap set.

The offscreen export device is the authority for:

- external frame tokens, output generations, and diagnostic slot indices;
- DMA-BUF export metadata;
- exact Vulkan external-ownership state;
- producer acquire fences and consumer release fences;
- delayed swap completion and bounded backpressure;
- the Mojo output endpoint.

Noctalia remains the authority for:

- importing and caching each DMA-BUF;
- waiting on the producer dependency;
- Graphite sampling completion;
- returning the token-correlated release fence;
- Wayland presentation.

## Required invariants

1. Every exported DMA-BUF contains a complete current root frame.
2. A newly allocated, reshaped, or content-invalid buffer begins with
   full-surface framebuffer damage.
3. The selected buffer's redraw is the union of current root damage and every
   change accumulated since that buffer last represented global content.
4. A successful swap clears only the selected buffer's accumulated damage and
   unions current root damage into every other buffer.
5. The exported `damage_rect` describes change relative to the previously
   published global frame, not the selected buffer's catch-up redraw.
6. An empty root-damage frame does not make an incomplete buffer current.
7. Transparent pixels are preserved correctly. Damage that removes content
   must clear the affected pixels to transparent before or as part of drawing.
8. Pixel-moving filters, backdrop filters, shadows, anti-aliasing, and render
   pass dependencies expand redraw damage through Chromium's existing damage
   logic.
9. A root SharedImage is never selected for writing while its previous
   exported access remains held by Noctalia.
10. The present/swap completion for a buffer does not run until Noctalia has
    returned its GPU release fence.
11. The acquire fence, release fence, ownership-transfer layout pair, frame
    token, generation, mailbox, and damage all refer to the same publication.
12. Resize and output-generation retirement never transfer damage history
    from differently sized or formatted buffers.
13. Only one root plane is exported. Video and other candidates must remain
    composited into that root rather than disappearing into unsupported
    hardware overlay planes.
14. No CPU readback, CPU pixel copy, persistent destination-image copy, GLES
    path, or full-frame compatibility renderer is introduced.

## Preferred Chromium integration

### Reuse the renderer-owned `BufferQueue`

Keep `SkiaOutputDeviceOffscreenExport`, but change its role. It stops
allocating and painting root surfaces directly and becomes a surfaceless
export/presentation sink for a renderer-owned root mailbox.

Set `renderer_allocates_images = true` in its capabilities. `SkiaRenderer`
then creates Chromium's existing root `BufferQueue`, asks it for the current
mailbox before recording, and obtains `BufferQueue::CurrentBufferDamage()` on
the Viz sequence. This is important: the buffer choice and its accumulated
damage are decided in the same sequencing domain that computes the render
scissor.

The export device must set or preserve these capabilities:

```text
uses_default_gl_framebuffer = false
output_surface_origin = top-left
renderer_allocates_images = true
number_of_buffers = 4
max_pending_swaps = 3
supports_post_sub_buffer = true
supports_target_damage = true
backdrop_filters_replace_destination = true
RGBA_8888 and BGRA_8888 color-type mappings
```

The four-buffer pool and three-pending-swap limit preserve the existing
backpressure depth. Do not reduce it merely to make buffer-age bookkeeping
easier.

`BeginPaint()` and `EndPaint()` become unreachable for the root path because
Graphite paints into the renderer-owned mailbox. `ScheduleOverlays()` and
`Present()` become the output device's publication entry points.

Do not route the endpoint through the generic
`SkiaOutputDeviceBufferQueue`/`OutputPresenter` pair without first changing
that pair's access-lifetime contract. Its window-presenter implementation
caches `OverlayData` and passes a borrowed scoped-access pointer whose lifetime
is coupled to later overlay reuse. The CEF transport requires one exact scoped
access to remain correlated with one external frame token until Noctalia
returns that token's release fence. A specialized offscreen device can own
that association directly without widening every platform presenter API.

### Keep one composited root plane

`SkiaRenderer` schedules its renderer-owned root mailbox as a primary overlay
candidate. The candidate carries the root damage in
`OverlayCandidate::damage_rect`.

The offscreen export device must accept exactly that primary root candidate.
The offscreen root creation path must disable promotion of independent video
or UI overlay planes, or otherwise prove that the overlay processor cannot
produce them for the null offscreen surface. A second visual plane is a
protocol error; silently ignoring it would omit page content.

This preserves the existing one-RGBA-frame CEF contract. Hardware-decoded
video may use multiplanar SharedImages internally, but Viz composites it into
the exported root.

### Export from a publication-owned root access

`SkiaOutputDeviceOffscreenExport::ScheduleOverlays()` receives the root
candidate containing the renderer-owned mailbox. It creates an
`OverlayImageRepresentation` and starts one
`OverlayImageRepresentation::ScopedReadAccess` specifically for that
publication.

For the next `Present()` it records:

- root mailbox and stable per-generation slot index;
- coded size, visible rect, format, color space, alpha type, and top-left
  origin;
- the candidate's clipped integer root damage;
- the representation and scoped access that own the producer fence and Vulkan
  transfer state.

`Present(update_rect)` provides the same global root damage through the
partial-swap path. Validate it against the root candidate's damage after
clipping and coordinate conversion. A disagreement is a producer protocol
error, not a reason to guess or silently union in framebuffer catch-up damage.

At `Present()` it:

1. validates that exactly one root candidate was scheduled;
2. takes the producer acquire fence;
3. takes the exact external Vulkan ownership transfer;
4. obtains and exports the `NativePixmap`;
5. allocates a monotonically increasing frame token and content serial;
6. publishes the Mojo `OffscreenOutputFrame`;
7. reports publication-time synthetic presentation feedback;
8. retains the swap-completion callback and scoped-access association until
   consumer release.

The export device must not copy or retain borrowed plane FDs beyond the
Mojo/CEF callback contract. Noctalia continues duplicating an FD only on an
import cache miss.

### Complete swaps only after consumer release

`ReleaseFrame(frame_token, release_fence)` must:

1. find the exact pending publication;
2. reject duplicate, unknown, zero, or cross-generation tokens;
3. attach Noctalia's exact `GENERAL -> GENERAL`,
   `VK_QUEUE_FAMILY_EXTERNAL_KHR` release state to that scoped access;
4. attach the consumer release fence;
5. mark the publication consumer-complete;
6. drain swap completions in original submission order.

Completing the output-device swap ends the publication-owned overlay read
access. The renderer-owned `BufferQueue` then receives
`SwapBuffersComplete(true)` and makes that root buffer available for later
rendering with its accumulated framebuffer damage intact.

Out-of-order consumer releases are allowed, but swap callbacks remain FIFO.
The pool provides backpressure after three pending publications rather than
failing or overwriting a held buffer.

### Generation and slot identity

`Reshape()` increments the output generation and starts a fresh mailbox-to-slot
mapping. Every newly observed root mailbox receives one stable slot index in
the range `[0, 3]` for that generation.

Pending publications from an old generation remain releasable by frame token.
Their callbacks and access objects must drain normally. They do not contribute
damage or slot identity to the new generation.

`content_serial` remains a monotonically increasing publication serial. It is
useful for identity and diagnostics, but it is not used to infer damage.

## Why not extend the existing export queue with another damage tracker

The current `OffscreenOutputQueue` selects a render slot on the GPU sequence in
`BeginPaint()`. Viz asks for framebuffer damage before that GPU task executes.
Mirroring the future slot choice back to the Viz sequence would require:

- reserving buffers ahead of recording;
- a new GPU-to-Viz damage side channel;
- careful ordering against asynchronous consumer releases;
- conservative full damage whenever the reservation and cached damage do not
  match.

That duplicates the responsibilities already implemented by Chromium's
renderer-owned `BufferQueue`. It also creates two independent state machines:
one for partial-swap validity and one for external ownership.

The preferred refactor removes that duplication. If source constraints make
the renderer-owned root-buffer route impossible, a separately reviewed
fallback design may use one persistent internal root plus damage-only GPU
copies into export slots. That is not the default because it adds a producer
copy and retains a second root allocation.

## Noctalia integration

### Initial landing

No CEF API extension is required. The current Mojo and CEF accelerated-paint
metadata already transports `damage_rect` as `capture_update_rect`.

For the first landing, Noctalia should:

- continue validating and importing the complete frame exactly as today;
- continue rebinding the scene texture to the newly published slot;
- continue redrawing and presenting the CEF surface for every changed frame;
- validate that a present damage rectangle is non-negative and contained in
  the coded bounds;
- not use damage as DMA-BUF import identity;
- not skip acquire, sampling, or release for a small or empty rectangle.

The producer optimization is effective even if Noctalia does not consume the
rectangle for local scissoring.

### Deferred Noctalia damage propagation

Do not combine this work with swapchain partial redraw.

Noctalia's Vulkan WSI also rotates images. Restricting its Graphite recording
to CEF damage would require the same buffer-age reasoning, plus expansion for
the panel's rounded clip, transforms, shadows, background blur, and any
content drawn above or below the CEF texture.

After producer-side results are measured, a separate plan may:

- carry CEF damage into scene-node invalidation;
- transform it into surface coordinates;
- expand it for sampling/filter footprints;
- union it with damage from every other scene node;
- track accumulated damage for each swapchain image;
- use scissored Graphite recording and `VK_KHR_incremental_present` only when
  the compositor/driver path benefits.

Drawing one textured quad is much cheaper than rerendering the web page, so
this is deliberately lower priority.

## Implementation phases

### Phase 0: preserve the current correctness baseline

Before changing ownership:

- keep the current full-root implementation buildable in the prior commit;
- retain the exact external Vulkan layout/fence protocol;
- retain Graphite/Dawn/Vulkan, transparent-root backdrop replacement, and
  four-slot backpressure;
- retain the hardware-video work independently;
- capture the current full-frame golden cases used by the offscreen exporter.

Do not ship a runtime switch or parallel backend. Git history is the rollback.

### Phase 1: make the offscreen endpoint renderer-owned

Refactor `SkiaOutputDeviceOffscreenExport` into the surfaceless sink described
above and enable renderer-owned root images.

Land with:

- four renderer-owned root buffers;
- no independent promoted overlay planes;
- full damage still forced if necessary;
- exact current DMA-BUF ownership and release behavior;
- current CEF and Noctalia APIs unchanged.

This phase proves the ownership/lifetime refactor before relying on partial
content preservation.

### Phase 2: enable Chromium partial swap

Enable `supports_post_sub_buffer` and `supports_target_damage`.

Verify that:

- `DirectRenderer::use_partial_swap_` becomes true;
- `BufferQueue::CurrentBufferDamage()` contributes to the root scissor;
- `BufferQueue::UpdateBufferDamage()` receives current root damage;
- initial and reshaped buffers carry full framebuffer damage;
- pixel-moving-filter expansion remains in Chromium's existing path;
- the export device publishes root damage rather than framebuffer catch-up
  damage.

Do not manually union buffer-age damage into the exported rectangle.

### Phase 3: finish transport and lifecycle edge cases

Cover:

- empty/no-damage frames;
- out-of-order consumer release;
- pool saturation and scheduler backpressure;
- resize while old buffers are published;
- hide/show and browser detach;
- output endpoint disconnect;
- Graphite/Dawn context loss;
- CEF renderer recovery;
- shutdown with pending publications.

No path may make an externally held root mailbox writable before its release
fence completes.

### Phase 4: test content preservation

Add deterministic rotating-buffer tests before performance measurement.

Use a surface divided into independently identifiable tiles. Across at least
eight frames, change non-overlapping tiles so every one of four buffers is
reused after missing several changes. Read back every exported frame and
compare the complete image, not just its declared damage.

Required cases:

- single small rectangle per frame;
- overlapping and disjoint rectangles;
- damage spanning buffer wraparound;
- content becoming fully transparent;
- transparent text and anti-aliased edges;
- rounded clips;
- linear and Gaussian filters;
- CSS-like backdrop-filter expansion;
- a pixel-moving filter whose output extends beyond input damage;
- resize and format/generation changes;
- an empty frame between damaged frames;
- release order different from publish order;
- one buffer held long enough to accumulate nearly full damage.

### Phase 5: live correctness acceptance

Build and package CEF, rebuild Noctalia, and run with Vulkan synchronization
validation and Dawn validation.

Exercise:

- Apple Music home, playlist, and album pages;
- continuously animated artwork;
- playback bar CSS backdrop blur over transparent page content;
- scroll wheel and scrollbar-thumb dragging;
- text input and history navigation;
- panel hide/show, resize, and reopen;
- fractional scale and the 120 Hz display;
- hardware and software video decode cases;
- GPU-process recovery and application shutdown.

Acceptance requires no stale tiles, trails, transparency residue, one-frame
reversions, flicker, yellow flashes, or missing video.

### Phase 6: performance acceptance

Compare the optimized release build against the prior full-root release build.
Validation must be disabled for performance numbers.

Record:

- `Compositing.DirectRenderer.TotalPixelsRendered`;
- root, framebuffer, total, and extra partial-swap damage percentages;
- CEF browser, renderer, and GPU-process CPU utilization;
- GPU render utilization and power state;
- Graphite/Dawn GPU duration where available;
- input-to-visible and queue-to-visible latency;
- changed frames, no-damage BeginFrames, and presentations;
- pending swaps, release latency, and pool saturation;
- CEF GPU-process memory and Noctalia import-cache stability.

Success means:

- small web changes render materially fewer root pixels;
- animated-cover pages reduce total root work without cadence loss;
- full-page transitions naturally approach full damage without regression;
- no additional CPU synchronization or full-frame copy appears;
- release latency and pending-swap pressure do not increase materially.

If Apple Music regularly produces near-full damage because of its own layer or
filter structure, preserve the correct implementation but reassess its
practical value before adding Noctalia-side damage complexity.

### Phase 7: cleanup and documentation

After acceptance:

- remove the direct root-allocation code made obsolete by the renderer-owned
  path;
- remove obsolete `OffscreenOutputQueue` state if external publication no
  longer needs it;
- keep only inexpensive aggregate health counters;
- remove temporary damage logging and readback code;
- update `cef-zero-copy-frame-architecture.md`;
- keep all Chromium/Viz changes in
  `0001-viz-offscreen-output-transport.patch`;
- keep CEF callback transport changes, if any, in
  `0002-cef-osr-dmabuf-and-frame-scheduling.patch`;
- do not add an overlapping patch file.

## Expected source ownership

The expected Chromium/Viz work is:

```text
components/viz/service/display_embedder/
  skia_output_device_offscreen_export.{h,cc}        refactor endpoint
  offscreen_output_queue.{h,cc}                     remove or reduce
  skia_output_device_offscreen_export_unittest.cc   damage/lifetime tests

components/viz/service/display/
  skia_renderer.*                                   ideally unchanged
  direct_renderer.*                                 unchanged

services/viz/privileged/mojom/compositing/
  offscreen_output.mojom                            ideally unchanged
```

CEF transport should ideally remain unchanged because it already forwards the
producer damage:

```text
libcef/browser/osr/
  render_widget_host_view_osr.*                     verify only
```

Noctalia changes for the first landing should be limited to validation and
tests:

```text
src/cef/
  cef_service.cpp
  cef_gpu_frame_bridge.*
```

Do not patch generated Chromium source directly. Regenerate the existing
Nucleus patch owners from a clean pinned source tree.

## Test matrix

### Unit/state tests

- four-buffer allocation and wraparound;
- initial full damage;
- accumulated damage per buffer;
- successful swap clearing only the current buffer;
- failed swap conservatively restoring full damage;
- FIFO completion with out-of-order consumer release;
- token uniqueness and duplicate-release rejection;
- generation retirement and old-token release;
- pool saturation without overwrite;
- output disconnect with pending frames.

### GPU pixel tests

- complete-frame equality for every rotating buffer;
- RGBA and BGRA;
- premultiplied transparency;
- stale-pixel detection using alternating high-contrast colors;
- transparent removal;
- filter-expanded damage;
- Graphite/Dawn SharedImage access and fence round trip.

### CEF integration tests

- `capture_update_rect` is smaller than the coded size for a small web change;
- the buffer outside `capture_update_rect` still matches the prior global
  frame;
- a buffer reused after three different partial updates contains all three;
- CPU `OnPaint` remains unused;
- every accelerated frame receives exactly one release disposition.

### Live acceptance

- animated artwork remains continuous and complete;
- playback and page navigation remain smooth;
- CSS blur and transparent-root behavior remain correct;
- authentication and browser state survive reopen;
- FD, SharedImage, mailbox, and import counts remain bounded;
- Vulkan/Dawn validation stays clean.

## Non-goals

This work does not:

- enable or diagnose hardware video decoding;
- change media color conversion or YUV plane export;
- fix an existing animated-art flicker by assumption;
- add damage-based Wayland presentation to Noctalia;
- add `VK_KHR_incremental_present` immediately;
- add a CPU, copied-frame, Ganesh, GL, or disabled-CEF fallback;
- infer correctness from inode, FD number, slot number, or timing;
- retain both direct-root and renderer-owned-root exporters in production.

## Relationship to other work

Hardware video decode and partial root damage optimize different stages:

```text
hardware decode:
  compressed H.264 -> decoded video frame

partial damage:
  Chromium/Viz page composition -> exported RGBA root frame
```

Finish hardware-decode correctness first so decoder selection, decoded-surface
interop, and root-composition cost are not conflated. Partial damage remains
valuable whether an animated cover is decoded in software or hardware.

The Vulkan access-state refactor remains a prerequisite. Buffer reuse makes
content preservation more important, not less: an initialized buffer cannot
be reacquired with discard semantics or an inferred ownership state.

The authoritative neighboring documents are:

- `cef-zero-copy-frame-architecture.md` for the deployed end-to-end frame
  contract;
- `chromium-ozone-vulkan-access-state-plan.md` for initialized-image ownership
  preservation;
- `cef-nvidia-hardware-video-decode-plan.md` for the independent media decode
  workstream.

## Completion criterion

The work is complete when:

1. Chromium's native `BufferQueue` is the sole authority for root buffer age
   and accumulated damage;
2. the CEF endpoint exports the true root damage while every DMA-BUF remains a
   complete current frame;
3. no externally held buffer is rewritten before its release fence;
4. rotating-buffer pixel tests pass for transparency, filters, resize, and
   out-of-order release;
5. Vulkan and Dawn validation remain clean;
6. live Apple Music behavior has no visual or interaction regression;
7. measured root pixels and producer work decrease on partial-update content;
8. the old full-root direct exporter and temporary diagnostics are removed;
9. the authoritative architecture documentation describes the new path.
