# CEF Zero-Copy Frame Architecture

Last updated: 2026-07-16

## Decision

Noctalia embeds Apple Music with CEF Alloy windowless rendering and consumes
accelerated DMA-BUF frames directly as Skia Graphite textures. This is the only
CEF rendering path. There is no GLES, CPU-paint, persistent-image-copy, or
runtime compatibility fallback.

The workspace is pinned to CEF branch 7922 and Chromium 151.0.7922.19. CEF is
built from source with Chrome FFmpeg branding and proprietary codecs so AAC,
H.264, and Widevine-backed Apple Music playback remain available.

## Frame contract

CEF calls `OnAcceleratedPaint` with a borrowed DMA-BUF description, an acquire
sync FD, and a frame token. `CefGpuFrameBridge::acceptFrame` validates the
format, modifier, plane layout, dimensions, and Vulkan external-memory support.
It duplicates the borrowed DMA-BUF descriptor only when creating a cached
modifier-backed Vulkan import.

Imports are keyed by stable file identity plus image geometry and layout. Viz's
direct-output device owns four persistent export slots, so the import cache is
bounded to that explicit queue rather than Chromium video capture's variable
pool depth. Each entry
owns its imported `VkImage`, dedicated `VkDeviceMemory`, Graphite
`BackendTexture`, and `SkImage`.

The bridge exposes one generation-safe `TextureHandle`. When a new CEF buffer
arrives, `GraphiteTextureManager::rebindExternalImage` changes the handle's
backing `SkImage` without changing the scene node or texture ID.

Before Graphite samples the image, the renderer:

1. imports CEF's acquire sync FD into the frame slot's Vulkan semaphore;
2. waits on that semaphore on the shared graphics queue;
3. transfers ownership from `VK_QUEUE_FAMILY_FOREIGN_EXT` to the Graphite
   graphics queue while keeping the image in `VK_IMAGE_LAYOUT_GENERAL`.

After Graphite submits the recording, the bridge:

1. transfers the source image back to `VK_QUEUE_FAMILY_FOREIGN_EXT`;
2. signals an exportable Vulkan semaphore;
3. exports that signal as a sync FD;
4. calls CEF's token-correlated
   `SetAcceleratedPaintReleaseFence(capture_counter, fd)` API.

Viz retains the slot's overlay read access until CEF returns that release
fence. Therefore Chromium cannot repaint or recycle a DMA-BUF while Noctalia is
sampling it. A superseded or hidden frame still receives a balanced
acquire/release sequence before recycling.

Presentation does not extend the source-buffer lifetime: after Graphite has
rendered the browser into the swapchain image, `vkQueuePresentKHR` reads the
swapchain image rather than the CEF DMA-BUF.

## Threading and scheduling

CEF uses its external message pump on Noctalia's main thread. Visible browser
surfaces use mandatory external begin frames driven by the owning Wayland
surface's `wl_surface.frame` callback. Input may request a frame immediately.
The custom timed begin-frame API carries an exact nanosecond refresh interval
and a deadline relative to the call. CEF reports the matching Chromium
`BeginFrameAck`, including its `has_damage` value, and that acknowledgment is
the only event that completes Noctalia's in-flight scheduler state. Accelerated
paint delivery never substitutes for acknowledgment.

`OnScheduleMessagePumpWork` may originate on any CEF thread. Its callback owns
only shared deadline state and wakes Noctalia's poll loop; it never captures
the poll-source object or dereferences `CefService::Impl` across shutdown.
Service shutdown disables that callback through a synchronized bridge before
calling `CefShutdown`. After each dispatch, the poll source retains CEF's
reference external-pump maximum delay of 33 ms. This low-frequency safety pump
is separate from external begin-frame cadence and is not an animation timer.

Noctalia keeps only the newest Wayland opportunity while a request is in
flight. The real acknowledgment either releases that opportunity immediately
against a freshly predicted presentation deadline or returns the scheduler to
idle. Four consecutive no-damage acknowledgments suppress full-rate idle work;
a 250 ms idle probe detects newly autonomous animation. A two-second watchdog
is recovery-only protection for a broken acknowledgment contract and never
acts as an animation clock. It requests invalidation but deliberately preserves
the accepted in-flight state until the real acknowledgment or a lifecycle
generation change. Hidden or detached panels call CEF's hidden-state API,
invalidate the scheduler generation, and stop all begin-frame work.

`wp_presentation_feedback` supplies the realized timestamp, sequence and exact
refresh interval used to predict the next deadline. The integer CEF
`windowless_frame_rate` remains only a maximum capture-rate hint.

The Apple Music surface arms callback-only `wl_surface.frame` ticks only while
the acknowledged scheduler reports demand. Active or in-flight work keeps the
chain alive. The fourth consecutive no-damage acknowledgment stops it; input,
navigation, resize, renderer recovery, or a damage-producing idle probe arms
one callback and restarts the compositor-paced chain.

Pointer motion is still coalesced to the newest integer-pixel position, but
queueing a new position independently arms one callback tick. This input wake
edge is required even when no-damage suppression says animation has no current
demand: the callback forwards the position before requesting an urgent begin
frame. Without it, an overlay-scrollbar drag can retain its newest position
until button release, while wheel input appears correct because wheel events
flush pointer motion immediately.

Graphite recording, texture rebinding, Vulkan queue submission, and CEF frame
acceptance all occur on the main thread. Background workers remain CPU-only.

## Superseded deployed path

The SDK installed before this cutover used `CefVideoConsumerOSR`, which creates a
`ClientFrameSinkVideoCapturer`. For every accepted capture, Viz reserves a
SharedImage from its video-frame pool and issues a `CopyOutputRequest` with a
`BlitRequest` into that image. The request selects the complete content
rectangle even when Chromium reports a smaller damage rectangle. The upstream
source explicitly notes that partial selection requires tracking the content
version held by each rotating pool image.

Consequently, the superseded path was:

```text
Blink/renderer compositor frame
  -> Viz final render passes
  -> full-surface Viz capture blit into an exportable SharedImage
  -> CEF accelerated-paint callback
  -> direct Graphite sampling
  -> Noctalia Vulkan swapchain
  -> Wayland compositor
```

A normal Chrome Wayland window does not take the video-capture branch: Viz
renders into a presentable output buffer and hands it to the compositor. This
extra OSR capture stage is the remaining architectural baseline gap. It does
not explain long renderer-main stalls from JavaScript, style, layout, or
garbage collection, but it adds work and at least one synchronization boundary
to every delivered frame.

That diagnosis established the concrete Chromium seam without replacing the renderer
compositor. `CefRenderWidgetHostViewOSR` already owns a `ui::Compositor` with a
null accelerated widget. Viz consequently renders the aggregated root pass
through `SkiaOutputDeviceOffscreen`, whose current implementation owns one
private backend texture and acknowledges `Present()` without exporting it.
`CefVideoConsumerOSR` was then started separately and captured that root frame
through `FrameSinkVideoCapturerImpl`. The cutover source now converts the
CEF-owned offscreen output device from one private backbuffer to a releasable
exportable buffer queue and does not start that separate video consumer for
Linux accelerated OSR. The old SDK remains runnable only until the replacement
build passes the real-panel acceptance gate.

## Scheduler refinement

The refresh clock remains Wayland, not a repeating Noctalia timer. The CEF
message-pump timer is a separate Chromium task-loop requirement; its bounded
33 ms fallback follows CEF's reference external-pump implementation and must
not be used as an animation clock.

The original Apple Music integration kept a callback-only `wl_surface.frame`
chain alive while attached even after CEF stopped full-rate begin frames. The
implemented contract replaces that unconditional chain with explicit demand:

1. `CefExternalFrameScheduler` exposes whether it needs another
   compositor-paced opportunity.
2. `CefService` owns a dedicated callback for arming one Wayland frame
   opportunity. Do not overload accelerated-paint readiness, which requests a
   real redraw.
3. The callback chain stays armed while animation is producing damage, a begin
   frame is in flight, or a newer opportunity is pending.
4. Callback arming stops once the acknowledged no-damage threshold enters
   quiescence.
5. Pointer/keyboard input, navigation, resize, explicit invalidation, renderer
   recovery, and an idle probe that discovers damage restart the chain.
6. The scheduler preserves one acknowledged begin frame in flight, coalesces
   opportunities, and continues deriving `interval_ns` and the next deadline from
   `wp_presentation_feedback`.
7. Behavioral tests cover active 60/120 Hz animation, no-damage idle, delayed acknowledgments,
   attach/detach, output refresh changes, and urgent wake-up behavior as state
   transitions rather than wall-clock sleeps.

The 250 ms quiescent probe remains intentional. With external begin frames,
`requestAnimationFrame` cannot independently wake a completely stopped client;
the low-rate probe lets autonomous page animation become visible again without
turning a timer into the steady-state refresh driver.

## Deeper OSR architecture

The principled end state is an exportable OSR output-buffer queue, not direct
sampling of Viz's existing internal root render target. Internal render-pass
textures are transient, may be reused immediately, and are not guaranteed to
have a Linux native-pixmap backing suitable for DMA-BUF export.

### Concrete implementation design

This design is based on the Chromium 151 implementation currently built by
Nucleus:

- `CefRenderWidgetHostViewOSR` creates a `ui::Compositor` with a null
  accelerated widget and owns the root OSR frame sink.
- GPU-composited null-widget displays select `SkiaOutputDeviceOffscreen` in
  `SkiaOutputSurfaceImplOnGpu::InitializeForVulkan()`.
- That output device renders into one private Skia backend texture and
  immediately acknowledges `Present()`.
- Independently, `CefVideoConsumerOSR` creates a
  `ClientFrameSinkVideoCapturer`. `FrameSinkVideoCapturerImpl` reserves a
  capture-pool SharedImage and sends a full-content `CopyOutputRequest` with a
  `BlitRequest` to populate it.

The replacement is a sibling output device, provisionally named
`SkiaOutputDeviceOffscreenExport`, selected only for an explicitly requested
exportable offscreen root. It renders directly into a bounded queue of
native-pixmap SharedImages and publishes the completed queue image. It must not
modify the ordinary `SkiaOutputDeviceOffscreen` used by headless Chromium,
tests, or other embedders.

This work does not require CEF and Noctalia to use the same Skia rendering
backend internally. The queue is built at Chromium's SharedImage boundary:
Chromium may use the supported Viz Skia backend for the pinned branch, while
Noctalia continues to import the resulting Vulkan DMA-BUF into Graphite. Moving
CEF from Ganesh to Graphite, if desired later, is a separate measurement and
compatibility decision.

### Process and Mojo boundary

The export device lives in the Viz GPU process, while
`CefRenderWidgetHostViewOSR`, the CEF render handler, and
`OnAcceleratedPaint` live in the browser process. The frame and release
messages therefore need an explicit bidirectional Mojo contract. Do not send
raw CEF types into `components/viz`.

Add general privileged Viz interfaces with names equivalent to:

```text
OffscreenOutputClient
  OnFrameAvailable(OffscreenOutputFrame frame)
  OnOutputError(OffscreenOutputError error)

OffscreenOutput
  ReleaseFrame(uint64 frame_token, PlatformHandle release_fence)
```

`OffscreenOutputFrame` contains:

- a monotonically increasing 64-bit frame token and queue-generation ID;
- coded size, visible/content rectangle, and damage rectangle;
- SharedImage format, color space, alpha type, and canonical image origin;
- the exported `NativePixmapHandle`, including every plane FD, stride, offset,
  size, DRM modifier, and producer acquire fence;
- the Viz frame timestamp and presentation/swap trace ID needed to correlate
  profiling events.

The following plumbing carries those endpoints without making Viz depend on
CEF:

1. Extend `ui::CompositorDelegate` with an exportable-offscreen request and
   endpoint factory. The default remains disabled.
2. `CefRenderWidgetHostViewOSR` requests it only for Linux accelerated OSR.
3. `VizProcessTransportFactory` places the client remote, output receiver, and
   request bit in `RootCompositorFrameSinkParams`.
4. `RootCompositorFrameSinkImpl` passes the endpoints through
   `OutputSurfaceProvider` and `SkiaOutputSurfaceImpl` to the GPU-side export
   device.
5. `CefRenderWidgetHostViewOSR` implements `OffscreenOutputClient` and owns the
   browser-side `OffscreenOutput` remote.

The dedicated interface is preferable to adding Linux DMA-BUF fields to the
general `DisplayClient`: it keeps exported-frame lifetime, release, and error
handling on one associated contract and leaves normal window presentation
untouched.

The expected source ownership is:

| Source area | Responsibility |
| --- | --- |
| `services/viz/privileged/mojom/compositing` | General frame, client, release, and error Mojo types; root-frame-sink endpoints |
| `ui/compositor/compositor.h` | Opt-in delegate hooks, disabled for ordinary compositors |
| `content/browser/compositor/viz_process_transport_factory.cc` | Bind the browser/Viz endpoints for the selected root compositor |
| `components/viz/service/frame_sinks/root_compositor_frame_sink_impl.*` | Carry the endpoints into output-surface creation and close them on root-sink loss |
| `components/viz/service/display_embedder/output_surface_provider*` | Select exportable output only for an offscreen GPU-composited root that explicitly requested it |
| `components/viz/service/display_embedder/skia_output_surface_impl*` | Pass SharedImage factories, context, memory tracking, and the export endpoints to the device |
| new `skia_output_device_offscreen_export.*` | Own the queue, SharedImage access scopes, swap backpressure, generations, and token map |
| `cef/libcef/browser/osr/render_widget_host_view_osr.*` | Request the direct path, translate Viz frames into accelerated-paint callbacks, return release fences, and stop the Linux accelerated video consumer |

The generic Chromium/Viz files must use embedder-neutral terminology. CEF- or
application-specific behavior remains under `cef/libcef/browser/osr`; no
Noctalia-specific language or policy belongs in the Chromium patch.

On receipt, `CefRenderWidgetHostViewOSR` registers the token as pending before
calling `CefRenderWidgetHostViewOSR::OnAcceleratedPaint`. It translates the
general frame metadata into the existing CEF accelerated-paint structure.
The exported plane and acquire-fence descriptors are borrowed only for that
synchronous callback. CEF's existing capture-counter field carries the direct
output frame token, preserving the public client API.

`SetAcceleratedPaintReleaseFence(token, fd)` then routes through
`CefRenderWidgetHostViewOSR` to the host display client and sends
`ReleaseFrame` to Viz. It no longer completes a
`FrameSinkVideoConsumerFrameCallbacks` object. Unknown, duplicate, stale-
generation, and invalid-fence releases are protocol errors; their descriptors
are closed and they never make a slot reusable.

### Queue allocation and slot contents

Use four buffers initially. Four allows one buffer to be rendered, one to be
delivered or sampled, and two to absorb browser/Viz/Noctalia scheduling skew at
120 Hz without inheriting the video capturer's eleven-buffer upper bound. The
count is a compile-time production constant for the first implementation, not
a user setting. Change it only from queue-depth traces.

Noctalia intentionally retains the currently displayed DMA-BUF after its first
sample. The shell may redraw the same scene texture without receiving another
CEF paint, so releasing it immediately would allow Chromium to overwrite
visible content. `CefGpuFrameBridge::acceptFrame` releases that held image only
while atomically rebinding its replacement, or the lifecycle discard path
releases it on detach. The output queue must therefore be able to produce a
replacement while one older frame remains published; a single-buffer queue or
one-pending-swap policy would deadlock this contract.

Each slot owns:

- a SharedImage mailbox allocated with display read/write and scanout/export
  usage so Ozone selects a native-pixmap backing;
- a persistent Skia SharedImage representation used for Viz writes;
- a persistent overlay SharedImage representation used for external reads;
- the current Skia write-access scope and wrapped `SkSurface`, only while
  rendering;
- the external overlay read-access scope, only while published;
- its generation, last frame token, content version, accumulated damage, and
  memory-accounting size.

Allocation follows the proven Ozone SharedImage path rather than creating a
private Skia backend texture and trying to export it afterward. Fail creation
if the selected Vulkan device cannot allocate a native-pixmap SharedImage in
BGRA8 or RGBA8 with a supported modifier. There is no CPU, GL, linear-image, or
video-capturer fallback for a requested exportable output.

`SkiaOutputDeviceBufferQueue` and `BufferQueue` are useful behavioral models,
but the presenter-oriented device should not be repurposed directly. The new
device has an external consumer and release fence instead of an
`OutputPresenter`; keeping that distinction explicit prevents window-system
overlay lifetime assumptions from leaking into OSR.

### Slot state machine

Every slot has exactly one of these states:

```text
Available
  -> Rendering
  -> Published(token)
  -> Available

Published(token)
  -> Retired(token)       on reshape, hide, or output replacement
  -> destroyed            only after the matching release

Any state
  -> Abandoned            on GPU context loss or channel teardown
```

The transitions are:

1. `BeginPaint()` takes an `Available` slot, begins SharedImage Skia write
   access, wraps that access as the output `SkSurface`, and marks it
   `Rendering`.
2. `EndPaint()` flushes the final root-pass writes and closes the write access.
3. `Present()` begins overlay read access on the same SharedImage. That access
   supplies the producer-completion fence and a native pixmap. The device
   exports both, allocates a never-reused frame token, stores the token-to-slot
   mapping, marks the slot `Published`, and sends `OnFrameAvailable`.
4. `ReleaseFrame()` sets Noctalia's release fence on the held overlay read
   access and destroys that access scope. Chromium's SharedImage backing then
   owns the release dependency. Released swaps drain in publication order;
   only when a token reaches that ordered head does its slot become
   `Available`, so an out-of-order release cannot let a new frame overwrite
   older completion bookkeeping. Its next write waits on the fence on the GPU
   rather than on a CPU thread.

The token map is installed before the Mojo frame notification is sent. Tokens
are monotonically increasing for the lifetime of the output connection and
are not derived solely from a slot index, preventing a late release from
aliasing a reused slot. A new GPU process creates a new Mojo connection and
generation, so releases from the dead connection cannot reach the replacement
queue.

### Synchronization contract

The queue must use
`gpu::OverlayImageRepresentation::ScopedReadAccess`, not duplicate its fence
logic manually:

- `BeginScopedReadAccess()` establishes external read access after the Viz
  write and provides the producer acquire `GpuFenceHandle` that is transferred
  with the native pixmap.
- Noctalia imports that sync FD, performs its existing
  foreign-to-graphics ownership acquire, and samples the image.
- Noctalia exports its graphics-to-foreign release as a sync FD and returns it
  with the token.
- `ScopedReadAccess::SetReleaseFence()` followed by destruction passes that
  dependency into `OzoneImageBacking::EndAccess`.
- A later Skia write access waits for the returned release fence on the GPU.

This closes the complete producer-to-consumer-to-producer chain without
`poll()`, `vkWaitForFences`, a CEF UI-thread stall, or a second Vulkan
allocation/import protocol inside Chromium.

A missing release fence is not permission to recycle a buffer. A watchdog may
record the stuck token and trigger bounded GPU/OSR recovery, but must never
force an unsignaled slot back to `Available`. On Mojo disconnect or GPU context
loss, destroy/abandon the entire queue rather than reusing uncertain storage.

### Swap backpressure and scheduling

`Present()` starts a Viz swap but does not complete that swap until the
corresponding client release returns. Set the output capabilities to four
buffers and at most three pending swaps, preserving at least one renderable
slot. This lets existing Viz pending-swap throttling apply backpressure before
`BeginPaint()` can run without an available image.

In steady state, frame N remains published as Noctalia's displayed texture
while Viz produces frame N+1. Accepting N+1 rebinds the scene texture and
returns N's release fence, which completes N's pending swap. No stage waits for
N to be released before it is allowed to produce N+1.

When the queue is saturated:

- do not block the browser UI thread, Viz thread, or GPU thread;
- do not allocate an emergency buffer;
- do not overwrite the oldest published buffer;
- let Viz coalesce compositor damage while pending-swap throttling prevents a
  new root draw;
- draw the newest aggregate state when a release completes and a new begin
  frame is accepted.

External begin frames remain presentation-driven by Noctalia. Queue release is
a backpressure signal, not another animation timer. The begin-frame
acknowledgment contract remains independent from accelerated-paint delivery.

The first correct version forces complete root damage for every selected queue
slot. Rotating buffers do not initially contain identical history, so applying
only the current frame's damage could expose stale pixels. This still removes
the separate full-frame capture blit; it merely asks Viz's existing root draw
to produce a complete valid output.

Concretely, the initial output device does not advertise post-sub-buffer,
target-damage, or output-device damage-reporting support. It treats every
`Present()` update as the full image and requires a full root redraw after
allocation or slot rotation. Tests must prove that Viz honors those
capabilities; merely replacing the callback damage rectangle after rendering
would be too late to repair stale slot contents.

### Later priority: per-slot damage optimization

This is explicitly outside the first direct-output implementation and
acceptance gate. Start it only after the capture blit is gone, the full-redraw
queue is correct under Vulkan validation, and an optimized trace shows both:

- Viz's root draw remains material to the frame budget, such as more than
  0.5 ms at p95 or 10% of GPU frame time; and
- at least 75% of damaged frames affect less than half of the output area
  after Viz has expanded damage for compositor effects.

If either condition is false, keep full redraws. Damage tracking adds state and
correctness surface but cannot improve JavaScript, style, layout, Noctalia's
final scene sampling, or animations that already damage most of the page.

#### Reuse Chromium's target-damage model

Do not add a CEF API or another Mojo message for this optimization. Chromium
already has the required rendering concepts:

1. Normal `BufferQueue` users accumulate damage separately for every rotating
   buffer and expose `CurrentBufferDamage()`.
2. GPU-owned output devices such as `SkiaOutputDeviceVulkan` keep a
   `damage_of_images_` entry per swapchain image.
3. Such a device advertises `supports_post_sub_buffer`,
   `supports_target_damage`, and
   `damage_area_from_skia_output_device`.
4. `FinishSwapBuffers()` places the damage of the next selected target in
   `SwapBuffersCompleteParams::frame_buffer_damage_area`.
5. `SkiaOutputSurfaceImpl::DidSwapBuffersComplete()` stores that value.
6. `SkiaRenderer::GetCurrentFramebufferDamage()` returns it before the next
   aggregation, allowing `Display` and `SurfaceAggregator` to include the
   target's stale regions in the next root draw.

Do not copy the `SkiaOutputDeviceVulkan` mechanism literally. That device
allows one pending swap, so one unversioned
`damage_of_current_buffer_` value is sufficient. The CEF export queue must
allow multiple pending frames because Noctalia retains frame N until frame N+1
is delivered. A late swap acknowledgment could otherwise describe a different
slot from the one selected for the next draw.

Keep actual SharedImages and access scopes in the GPU-side export device, but
add a small Viz-sequence `OffscreenExportTargetTracker` that mirrors only:

```text
generation
slot_id
slot_state
accumulated_damage
reservation_serial
```

Before aggregation, `ReserveNextTarget()` removes one available slot from the
tracker and returns `{generation, slot_id, reservation_serial, damage}`.
`SkiaRenderer::GetCurrentFramebufferDamage()` uses that reservation's damage,
and the reservation identity is sent with the ordered Skia output-surface GPU
task that will call `BeginPaint()`. `SkiaOutputDeviceOffscreenExport` validates
the generation, slot, and serial before beginning write access to that exact
image.

Publication, failed draw, release, and generation-retirement results return
through the existing Skia output-surface GPU-to-Viz completion path and update
the tracker. A failed or mismatched reservation restores the slot with full
damage. Swap completions remain FIFO even if external release messages arrive
out of order; a released later token is recorded but cannot overtake an earlier
pending swap.

This is an internal Viz/output-surface extension, not a public transport
change. It follows Chromium's normal front-end `BufferQueue` ownership of
target-damage decisions while leaving Vulkan objects and external-fence access
on the GPU sequence.

The current Noctalia contract remains unchanged. The accelerated-paint damage
rectangle describes the pixels rendered for the delivered frame, while the
internal per-slot history exists only to ensure those pixels were sufficient
to update the selected export image.

#### Damage state and algorithm

Each queue generation owns a monotonically increasing content version. In
addition to its synchronization state, every slot stores:

```text
valid_contents
last_content_version
accumulated_damage
last_full_redraw_reason
```

Use one clipped `gfx::Rect` initially, matching Chromium's normal
`BufferQueue`. Do not begin with a region, tile map, or DOM-derived damage
system.

The algorithm is:

1. New or reshaped slots start invalid with full-surface accumulated damage.
2. Before aggregation, select the exact `Available` slot that the next
   `BeginPaint()` will use and report its accumulated damage through Chromium's
   existing output-device damage channel.
3. Viz combines that historical target damage with the current aggregated root
   damage. Use Viz's final root damage; never attempt to infer changes from DOM,
   CSS, CEF callback bounds, or Apple Music behavior.
4. Render the combined damage directly into the selected slot. Published and
   retired slots are never modified.
5. After a successful `Present()`, increment the generation content version,
   mark the rendered slot valid at that version, and clear only its accumulated
   damage.
6. Union the final root damage into every other live slot, including available,
   published, and release-received slots that have not yet been selected for a
   new write.
7. If the draw or publication is skipped, canceled, or fails, do not advance
   the slot's version or clear its damage. Conservatively union full-surface
   damage when its contents can no longer be proven.

For four slots A through D, if A contains version 1 and frames 2 through 5
change the playback button, animated artwork, and progress bar, reusing A for
version 5 redraws the union of all those changes. Drawing only frame 5's
immediate artwork damage would leave A's other pixels at version 1.

The selected-slot decision must be stable across the damage report,
aggregation, `BeginPaint()`, and `Present()`. If availability changes or that
reservation cannot be honored, abandon the partial draw and promote the frame
to full damage rather than rendering into a different slot with unrelated
history.

#### Full-damage promotion rules

Initialize or promote a slot to full damage for:

- allocation, generation change, resize, or restoration after discard;
- format, color space, alpha type, sample count, image origin, transform, or
  device-scale change;
- invalid, missing, or discontinuous content-version history;
- failed/skipped draw or swap where preserved contents are uncertain;
- GPU context loss, SharedImage recreation, or recovery from a protocol error;
- an accumulated rectangle that already covers the full surface; or
- any Viz path that cannot prove its reported root damage includes all pixels
  affected by a filter, backdrop filter, mask, transform, overlay, delegated
  ink trail, letterbox operation, or color conversion.

Do not automatically promote every CSS filter or backdrop filter. Viz normally
expands compositor damage through those effect bounds. The export device
accepts Viz's conservative final result and promotes only when that propagation
is unavailable or uncertain; independently shrinking Viz damage is forbidden.

Empty damage does not advance or publish a new content version unless Viz
requires an empty swap for protocol bookkeeping. A no-damage acknowledgment
continues to drive the existing external-frame scheduler independently.

#### Instrumentation and decision data

Add temporary trace counters before enabling partial draws:

- current root-damage pixels and percentage of the output;
- selected slot's historical-damage pixels and age in content versions;
- final combined target-damage pixels;
- full-redraw count grouped by promotion reason;
- bounding-rectangle inflation and frames where the union becomes full;
- slot reservation changes, queue saturation, and out-of-order releases;
- root draw GPU duration and bytes written, when the backend exposes them.

Retain only compact production counters for unexpected full-damage promotion,
invalid history, and reservation mismatch. If a single bounding rectangle is
usually close to full because distant changes inflate the union, first keep
full redraws. Consider a bounded `gfx::Region` or coarse tile mask only from
that evidence and only if Chromium's renderer can consume it without
converting it immediately back to one bounding rectangle.

#### Staged implementation

1. **D0 — Observe only.** With the full-redraw direct queue still active,
   record Viz root damage, simulated per-slot accumulated damage, slot age, and
   promotion reasons. Do not change rendered pixels.
2. **D1 — Pure state model.** Extract generation/slot damage accounting into a
   deterministic helper and test rotations, held displayed frames, coalesced
   frames, skipped swaps, out-of-order releases, and reshape generations.
3. **D2 — Wire target damage.** Add the Viz-side target tracker and ordered
   reservation identity, advertise post-sub-buffer and target-damage support,
   validate the reservation in the GPU device, and keep a development-only
   force-full-redraw comparison build. Production still has one configured
   path.
4. **D3 — Correctness validation.** Compare partial and forced-full output
   hashes/goldens for scrolling, caret blinking, two distant damage regions,
   animated artwork, shadows, clipping, transforms, transparent content, CSS
   filters/backdrop filters, popups, resize, fractional scale, and dropped or
   coalesced frames.
5. **D4 — Performance gate.** Run the real optimized Apple Music panel at the
   monitor's reported refresh interval. Keep the optimization only if it
   materially reduces root draw time or memory traffic without worsening
   input-to-visible or queue-to-visible p95/p99.
6. **D5 — Optional region refinement.** Consider multiple rectangles or tiles
   only if D4 proves rectangle-union inflation is the remaining material cost.

Partial damage is therefore a separable follow-up patch series. It must not
delay removal of the unconditional OSR capture blit, and it can be abandoned
without changing the direct-output transport or Noctalia's release-fence
contract.

### Resize, visibility, and teardown

`Reshape()` creates a new generation when size, format, color space, alpha
type, sample count, scale, or origin changes:

- available old slots are destroyed immediately;
- rendering is canceled before publication;
- published old slots become `Retired` and keep their SharedImage and overlay
  access until their release fence returns;
- callbacks from the new generation contain new tokens and full damage;
- Noctalia's existing texture rebinding imports the new geometry and retires
  its old cached import only after sampling completes.

Retired generations are expected to drain quickly. Keep a hard diagnostic
limit of the current generation plus two retired generations. Exceeding it is
a stuck-client protocol failure that triggers bounded OSR recovery; it is not
resolved by destroying in-use storage.

Hiding the panel stops external begin frames and new publication but does not
invalidate a frame already borrowed by Noctalia. Noctalia's existing discard
path returns a balanced release fence. Once all published slots return, the
hidden queue may discard its backbuffers. Showing the panel allocates or
restores a generation and requests one full-damage frame.

Orderly shutdown is:

1. stop external begin frames and frame delivery;
2. have Noctalia sample or discard every delivered token and return its release
   fence;
3. close the CEF/Viz export connection;
4. retire/destroy queue slots;
5. destroy the root compositor and then shut down CEF.

If the GPU process or Mojo connection dies, CEF reports renderer/GPU loss and
Noctalia invalidates the external texture generation. Neither side waits on
fences owned by the dead process.

### Rendering correctness

The exported image is the final aggregated root pass, after Chromium has
composited renderer content, popups, transforms, opacity, masks, color
conversion, CSS filters, and backdrop filters. This is why the design changes
the root output device instead of exporting a transient internal render pass.
Noctalia's compositor blur remains visible through pixels Chromium outputs
with transparency.

There is an important transparent-root qualification. Chromium implements the
Filter Effects draft literally: it copies the existing backdrop into a saved
layer, filters that copy, draws the element into the layer, and restores the
layer over the original backdrop with source-over composition. In
`SkiaRenderer::PrepareCanvasForRPDQ`, a backdrop-filtered render-pass draw quad
therefore uses `SkBlendMode::kSrcOver`; Skia's
`SkCanvasPriv::ScaledBackdropLayer` initializes the saved layer from the
filtered prior device.

That is visually stable when the page backdrop is opaque because the filtered
copy is also opaque and covers the original. It is not a useful flattened
representation for a transparent embedded root. Sparse content such as text
produces a partially transparent blurred copy, and source-over leaves the
original sharp glyphs visible underneath it. Opaque artwork produces an opaque
filtered result and does cover its original, explaining the observed
"artwork blurs, text only dims" split. Alpha-channel inspection of a minimal
transparent Chrome reproduction contains the sharp glyph silhouettes, ruling
out LCD text rasterization, Wayland, niri, and DMA-BUF transport as the cause.
The same page on an opaque root blurs both text and artwork.

This is intentional web-platform behavior rather than a missing synchronization
edge. Chromium's WPTs explicitly test the source-over rule, and changing it
globally would change CSS semantics. The export path instead needs an opt-in
flattening policy for content that will be composed over an external backdrop:

1. Add one internal boolean to `OutputSurface::Capabilities`, set directly by
   `SkiaOutputDeviceOffscreenExport`. Do not add it to the offscreen Mojo
   protocol and do not add version or capability negotiation.
2. Extend Skia's backdrop-initialized `SaveLayerRec` with an optional
   local-coordinate replacement path. Skia snapshots and filters the prior
   device as usual, then clears the prior device only within that path
   immediately before restoring the layer with the existing `kSrcOver` paint.
   Preserve the path through deferred recording and SKP serialization.
3. In `SkiaRenderer::PrepareCanvasForRPDQ`, set that path only when the export
   capability is enabled and the aggregated root is transparent. Build it from
   the exact transformed `backdrop_filter_bounds`, intersected with the
   render-pass visible rect and draw region. Do not use the broader
   `filter_bounds`, which may include foreground-filter expansion.
4. Apply the policy to every backdrop-filtered render pass contributing to
   that transparent exported root, including nested render passes. Keep the
   existing rounded clips, masks, element opacity, and filter output behavior.
5. Leave every ordinary on-screen, capture, software, opaque-root, and
   non-exported output surface on the standards-defined `kSrcOver` path.

Clearing the bounded prior destination removes the original unfiltered page
pixels only where they were copied into the backdrop layer; the normally
restored filtered premultiplied result then takes their place. This is safer
than changing the whole layer restore to `kSrc`: foreground-filter output may
extend beyond the backdrop border box, rounded antialiased edges need coverage
on both the clear and restore, and element opacity still belongs to the
existing restore paint. Fully transparent gaps remain transparent for
Noctalia/niri, while Chromium page content behind Apple Music's player and
navigation glass no longer survives as a sharp second copy. The operation adds
one bounded clear draw but no extra render pass, backdrop copy, readback, or
cross-queue synchronization.

The focused Viz test should construct a transparent root with both an opaque
image-like block and sparse glyph-like shapes behind a translucent rounded
backdrop-filter render pass. It must verify that:

- default Viz output retains the standards-defined source-over result;
- the offscreen-export policy removes the original sharp alpha silhouettes;
- opaque-root output is unchanged;
- nested backdrop filters, element opacity, rounded clipping, masks, and
  partial damage do not clear pixels outside their filter bounds.

Implementation status (2026-07-17): the bounded replacement path is implemented
and preserved through immediate drawing, deferred Skia recording, and SKP
serialization. Focused Skia tests and all 15 backdrop-filter Viz pixel-test
variants pass (with the existing software-only skip), including Ganesh/GL and
Graphite/Dawn coverage. The official PGO/ThinLTO CEF package rebuilt
successfully, Noctalia rebuilt with all 49 tests passing, and a live run
received an accelerated frame and loaded authenticated Apple Music Home.

The Apple Music z-index changes introduced while sharp text was incorrectly
suspected to be above the player have been removed, along with their target
probes and logging. The retained stylesheet contains only the transparent
document background and intentional navigation tint/filter styling. After the
Viz test passes and CEF is rebuilt, verify the player and navigation over
static text, scrolling text, animated artwork, menus, and modal surfaces.

This policy is the principled solution for Noctalia's current composition
model: niri owns the external panel blur, while Chromium owns filtering of its
page content. One premultiplied RGBA frame is sufficient for that division. If
the host backdrop is locally constant across Chromium's CSS blur kernel,
filtering premultiplied page content and later compositing it over the host is
exactly equivalent to filtering their combination. Niri has already
low-passed the desktop, so the host backdrop is smooth at this scale and the
remaining covariance error should be visually negligible.

An exact generic implementation for an arbitrary high-frequency future host
backdrop would still require either feeding that synchronized backdrop into
Viz before Chromium applies CSS filters or exporting Chromium's ordered
render/effect graph so the host compositor can evaluate backdrop filters after
composition. The former creates a frame-cycle with niri's compositor-owned
blur; the latter is effectively a layered compositor protocol rather than an
image export. Neither is justified for the Apple Music panel while niri
already supplies the desired smooth external blur. Keep them as theoretical
boundaries, not follow-on implementation work.

The callback contract presents one canonical top-left image. The output device
must make the Skia surface origin and exported metadata agree; Noctalia does
not regain a GL-style Y flip. Premultiplied alpha, BGRA/RGBA channel ordering,
color space, and visible/content rectangles require pixel goldens before the
video-capture path can be removed.

### Patch sequence

Keep the implementation reviewable as a small ordered series. Files touching
the same source region belong in the same patch.

1. **Trace the existing cost temporarily.** Skipped by decision after the
   official PGO/ThinLTO build produced an obvious interactive improvement; do
   not delay the architectural work for another baseline capture.
2. **Add the generic Viz transport.** Implemented in
   `0001-viz-offscreen-output-transport.patch`: embedder-neutral frame,
   release, metadata, error, and endpoint Mojo types; an opt-in compositor
   hook; paired endpoint validation; and ownership carried through the root
   frame sink into the Skia output-surface lifetime. Focused Viz tests prove
   that paired endpoints arrive intact and half-connected requests are
   rejected.
3. **Add `SkiaOutputDeviceOffscreenExport`.** Implemented with four native-pixmap
   SharedImage slots, the state machine, full-damage rendering, overlay
   acquire/release access, pending-swap backpressure, reshape generations,
   memory tracking, and deterministic queue tests.
4. **Wire CEF OSR to the export path.** Implemented through the CEF OSR
   compositor delegate: generic frames are translated into the existing
   accelerated-paint callback and release fences return to the matching Viz
   token. Linux accelerated OSR no longer creates `CefVideoConsumerOSR`;
   non-Vulkan export initialization fails rather than selecting another
   renderer.
5. **Prove the real panel.** Run Vulkan synchronization validation, sustained
   animated Apple Music, transparent/CSS-blur content, resize, fractional
   scale, hide/show, input, playback, and GPU-process recovery.
6. **Remove superseded Linux capture patches.** Complete. The capture-buffer
   native-handle selection, fresh-handle cloning, capture-pool blit changes,
   callback retention, and timeout wait were removed. Upstream video capture
   remains intact for platforms and CEF modes outside the requested Linux
   accelerated-OSR contract.
7. **Consider per-slot damage only from evidence.** Implement it only after the
   direct-output trace shows root redraw bandwidth remains material.

There is no runtime switch between direct output and the old Linux accelerated
capture path; the coordinated CEF and Noctalia sources implement one production
contract. Development comparison builds are separate SDK artifacts.

### Tests and measurable gate

Unit and component coverage must include:

- slot transitions, token uniqueness, duplicate/stale release rejection, and
  queue saturation;
- acquire-fence export and release-fence return through a fake native-pixmap
  SharedImage backing;
- no slot reuse before its release dependency is installed;
- pending-swap completion and newest-damage coalescing;
- reshape with published old-generation slots and bounded retired generations;
- hide/show, client disconnect, context loss, and teardown with outstanding
  frames;
- BGRA/RGBA, modifier/plane metadata, top-left orientation, premultiplied
  transparency, color space, and full-damage initialization;
- CEF translation preserving borrowed-FD lifetime and token correlation.

The implementation does not pass merely because pixels appear. The optimized
real-panel trace must show:

- zero `FrameSinkVideoCapturerImpl` or root `CopyOutputRequest` activity for
  Linux accelerated OSR;
- zero per-frame SharedImage or DMA-BUF allocation after a generation warms;
- zero CPU fence waits on CEF, browser UI, Viz, Noctalia main, or GPU threads;
- exactly one Viz root render followed by one external sample for each
  delivered damaged frame, with no intervening full-surface capture blit;
- stable queue depth, FD count, allocation count, and retired-generation count
  during at least five minutes of 120 Hz-capable animation and interaction;
- no Vulkan validation error, stale/duplicate token, forced recycle, pool
  overwrite, or unsignaled release;
- no animation freeze without pointer movement, no image-only flash, and no
  regression in auth, audio, Widevine, input, CSS/backdrop blur, transparency,
  or panel lifecycle;
- input-to-visible and queue-to-visible p95 no worse than the optimized current
  path, with the gap to a native Chrome Wayland window reduced. Record absolute
  p50/p95/p99 values rather than claiming success from subjective smoothness.

Only after this gate passes should the direct output SDK replace the current
capture-based SDK.

### Production optimization boundary

The optimized CEF baseline is an official Chromium build with `NDEBUG`,
`dcheck_always_on=false`, `enable_expensive_dchecks=false`, branch-matched
Chromium and V8 PGO profiles, ThinLTO, and LLD. Generated compile commands must
contain `OFFICIAL_BUILD`, `-O2`, `-fprofile-use`, and `-flto=thin`; this is
stronger evidence than merely recording the requested GN arguments.

Official Chromium still deliberately enables production security hardening:
libc++ extensive-mode checks, trap-mode C-array-bounds and missing-return
checks, virtual-call CFI, and indirect-call CFI. These are not debug DCHECKs or
an accidentally retained sanitizer build. They match Chromium's Linux official
configuration and remain enabled for the fair baseline. Disabling them would
create a nonstandard, weaker browser binary and is justified only by a trace
showing one of those checks materially contributes to the measured Apple Music
critical path.

The packaged SDK must also be published atomically. Build a complete versioned
staging directory, then rename it and atomically switch the `current` symlink.
Deleting and recopying a selected SDK in place can expose a wrapper from one
CEF revision beside `libcef.so` from another if packaging is interrupted. The
atomic directory publication ensures the wrapper, headers, resources, and
`libcef.so` become visible as one unit.

This preserves the public Noctalia contract. The change is isolated to how CEF
produces the DMA-BUF, so `CefGpuFrameBridge`, Graphite texture rebinding, and
the scene node do not gain a second rendering mode.

The resulting frame path is:

```text
Blink/renderer compositor frame
  -> Viz renders final root pass directly into an exportable queue image
  -> CEF accelerated-paint callback
  -> direct Graphite sampling
  -> Noctalia Vulkan swapchain
  -> Wayland compositor
```

Embedding a separate Chromium Wayland subsurface is not the target. Although
it would resemble Chrome's native presentation path, it would move clipping,
rounded corners, stacking, input, IME, transparency, and lifecycle across a
second Wayland surface boundary and would prevent the CEF content from behaving
as an ordinary Graphite scene node. The exportable Viz output queue removes the
capture copy while preserving Noctalia's composition model.

## Device selection and recovery

Noctalia selects a Vulkan 1.4 device whose queue family supports both graphics
and presentation to the active Wayland display. When CEF is enabled, the same
device must also support external-memory FD, DMA-BUF modifier images, sync FDs,
and foreign queue-family ownership.

Noctalia publishes the selected Vulkan device UUID and DRM render node before
CEF starts. The patched Chromium/ANGLE process uses that contract so producer
and consumer operate on the compositor-presenting GPU.

On `VK_ERROR_DEVICE_LOST`, Noctalia performs one complete rebuild: abandon the
old CEF imports without waiting on the lost device, detach swapchain targets,
recreate Vulkan and Graphite, invalidate texture generations, attach a fresh
CEF bridge, and request a new accelerated frame. A second device loss is a
fatal renderer error rather than an unbounded recovery loop.

A CEF renderer-process termination is handled independently from Vulkan device
loss. Noctalia immediately suspends external begin frames and discards any
not-yet-sampled DMA-BUF, then requests one cached reload through the existing
persistent request context. A real `OnRenderViewReady` callback restarts the
external scheduler and invalidates the view. If the replacement renderer
terminates before becoming ready, recovery stops instead of entering an
unbounded reload loop.

## CEF patch ownership

The maintained CEF patch stack lives in `nucleus-workspace/cef/patches` and
contains only production changes:

- native DMA-BUF accelerated-paint metadata, token-correlated release fences,
  and acknowledged presentation-timed external begin frames;
- generic Viz direct-output transport and the four-slot export device;
- Linux Vulkan DMA-BUF/export requirements;
- ANGLE/Chromium pinning to Noctalia's Vulkan device;
- preservation of OSR device scale;
- correct handling of an already-reaped child process;
- transparent hover tracks for Chromium's overlay scrollbars.

CEF's C/C++ translation layer and API hashes are generated from the active
checkout by `nucleus-workspace/cef/build.sh` before compilation. They are never
carried from one CEF branch to another or maintained as source patches.
The coordinated CEF and Noctalia sources define one API contract. CEF API
translation and hashes are regenerated from the patched checkout before every
build rather than inferred from a separately described capability registry.

## Operational acceptance

The supported acceptance path is the real Noctalia Apple Music panel, not a
standalone proof binary. Before replacing a known-good CEF distribution:

- build Noctalia in Release mode against the new codec-enabled distribution;
- start the real shell with Vulkan validation enabled for the first run;
- confirm authenticated Apple Music state survives restart and panel reopen;
- navigate with mouse and keyboard, including text-field focus and cursor
  shapes;
- play AAC/H.264/Widevine content with working audio;
- exercise resize, fractional scaling, close/reopen, and sustained playback;
- confirm rendering continues without pointer movement;
- inspect logs for Vulkan validation failures, release-fence timeouts, CEF CPU
  paint callbacks, device loss, and shutdown errors;
- verify the bridge shutdown summary balances accepted, sampled/discarded, and
  release-fence counts and keeps the import cache bounded.

After the Chromium 151 distribution and real panel pass this acceptance, remove
the obsolete Chromium 150 source/build checkout. Retain its packaged
distribution only for a short rollback window, then remove it as well.

## Profiling

Tracy remains optional instrumentation for real-shell captures. Useful zones
are accelerated-paint acceptance, cached import lookup, acquire-fence import,
direct ownership acquire/release, Graphite submission, presentation, and
input-to-visible latency. There are intentionally no Vulkan copy or copy-fence
wait zones in the final architecture.
