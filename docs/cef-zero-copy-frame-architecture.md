# CEF Zero-Copy Frame Architecture

Last updated: 2026-07-17

## Decision

Noctalia embeds Apple Music with CEF Alloy windowless rendering and consumes
accelerated DMA-BUF frames directly as Skia Graphite textures. This is the only
CEF rendering path. There is no CPU paint, GLES, persistent-image copy, or
runtime backend fallback.

The workspace pins CEF branch 7922 and Chromium 151.0.7922.19. The SDK is built
from source with the required codec support. Noctalia always requests native
Wayland and ANGLE Vulkan; those are build/runtime requirements, not user
selectable compatibility modes.

The integration rests on two invariants:

1. Every accepted external BeginFrame terminates exactly once, either by
   ordinary completion or an explicit in-place abort.
2. Every exported offscreen buffer remains immutable until the consumer's GPU
   release fence completes.

Timeouts may request recovery, but may not pretend either transaction
completed.

## End-to-end frame path

```text
Wayland frame callback
  -> CEF external BeginFrame
  -> Chromium/Viz root render
  -> exportable offscreen buffer queue
  -> OnAcceleratedPaint
  -> Vulkan DMA-BUF import
  -> Graphite sampling
  -> Noctalia Vulkan swapchain
  -> Wayland presentation
  -> token-correlated release fence returned to Viz
```

CEF publishes a borrowed DMA-BUF description, producer acquire fence,
transport epoch, capture counter, output generation, output slot, and content
serial. The frame descriptors are valid only for the callback. Noctalia
duplicates the DMA-BUF descriptor only when creating a cached import and never
retains CEF's borrowed descriptor.

The export device uses a bounded queue of complete root frames. Partial swap is
disabled, so each published slot is independently valid; future damage-based
rendering must preserve that property explicitly before it can replace the
full-root path.

## Consumer ownership and synchronization

`CefGpuFrameBridge` validates each frame's dimensions, format, modifier, plane
layout, producer layout, queue-family domain, and external-memory support.
Imports are cached by transport epoch, stable file identity, geometry, format,
modifier, layout, queue family, stride, and offset. Each cache entry owns:

- the imported `VkImage` and dedicated `VkDeviceMemory`;
- its Graphite `BackendTexture` and `SkImage`;
- the completion value of its latest GPU use.

The scene retains one generation-safe `TextureHandle`.
`GraphiteTextureManager::rebindExternalImage` changes its backing image when a
new CEF slot arrives without changing scene identity.

For a newly published frame:

1. Import the producer sync FD into the selected frame-slot semaphore.
2. Wait on that semaphore and record the producer-to-graphics ownership
   acquire plus the transition to
   `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
3. Signal acquire-complete and attach that exact wait to the Graphite
   recording that samples the image.
4. Have that recording signal sampling-complete.
5. Consume sampling-complete in a bridge submission and advance the completion
   timeline for the image.

The currently displayed buffer stays owned by Noctalia across unrelated scene
redraws. When its replacement is rebound, the bridge:

1. waits on the latest Graphite completion timeline value;
2. records the graphics-to-producer ownership release and restores the
   producer handoff layout;
3. signals an exportable semaphore;
4. exports its sync FD;
5. returns it through
   `SetAcceleratedPaintReleaseFence(transport_epoch, capture_counter, fd)`.

A frame superseded before it is drawn still receives a balanced acquire and
release. Viz cannot rewrite or recycle a slot until its matching release fence
signals.

Queue-family ownership must match across processes. Chromium carries the
producer's actual handoff domain in the frame metadata; Noctalia must not infer
`FOREIGN_EXT` or `EXTERNAL_KHR` locally. Vulkan validation cannot verify the
other process's half of this ownership transfer, so this remains an explicit
cross-process contract.

## External BeginFrame scheduling

The Wayland surface's `wl_surface.frame` callback is the visible animation
clock. It stays armed for the whole visible lifetime, including after
no-damage acknowledgements, because future web animation can create future
damage. Input, navigation, resize, and explicit invalidation may request an
urgent opportunity but do not sustain animation.

`wp_presentation_feedback` supplies the realized timestamp and refresh
interval used for deadline prediction. `windowless_frame_rate` is only a
capture-rate ceiling. The CEF external-message-pump deadline is a separate
Chromium task-loop requirement and is not a frame timer.

Only one external BeginFrame may be accepted at once. Noctalia coalesces later
Wayland opportunities until Chromium returns the matching acknowledgement.
Accelerated paint does not substitute for that acknowledgement.

An accepted request owns immutable `BeginFrameArgs`. CEF/Viz resolves its
one-shot completion callback on normal completion, explicit abort, display
destruction, controller disconnection, and teardown. Completion clears
transaction state before invoking the callback so reentrancy cannot complete a
newer request accidentally.

Panel detach stops new opportunities and enters a draining state. If a request
is in flight, Noctalia asks the existing Viz controller to abort that exact
request and delays `WasHidden(true)` until its ordinary completion callback.
Reopen during the drain records resume intent and starts a new scheduler
generation only after the old transaction terminates.

The watchdog is recovery-only. It requests an in-place abort and never destroys
the live output transport merely to clear local scheduler state.

## Threading

CEF uses the external message pump. Scheduler transitions, BeginFrame
completion, accelerated paint, texture rebinding, Graphite recording, and
Vulkan bridge submission are sequence-bound to Noctalia's main thread.

`OnScheduleMessagePumpWork` may originate on another CEF thread. It communicates
only a synchronized deadline and wakes the poll loop; it must not capture a
poll source or service implementation across shutdown.

Release callbacks are invoked after the bridge mutex is unlocked. CEF callback
boundaries do not allow exceptions to escape.

## Lifecycle and recovery

On normal hide or detach, any adopted frame is released through the normal GPU
fence path. On Vulkan device loss, Noctalia abandons device-local bridge
objects without submitting to the lost queue, invalidates the texture handle,
rebuilds the shared device and Graphite context, recreates the bridge, and
requests a fresh CEF frame.

CEF profile state is persistent and independent of the graphics device.
Browser recreation is last-resort containment for a terminal renderer/browser
failure, not ordinary scheduler recovery.

At shutdown:

1. disable cross-thread message-pump scheduling;
2. stop external BeginFrame opportunities;
3. close the browser and drain `OnBeforeClose`;
4. release the bridge;
5. call `CefShutdown`.

## Patch ownership

The Chromium/CEF patch stack owns only behavior that must exist inside the
producer:

- exportable Viz offscreen output and transparent-root backdrop composition;
- frame identity, queue-family/layout metadata, and release-fence plumbing;
- exact external-BeginFrame completion and in-place abort;
- timed external BeginFrame entry points;
- Linux DMA-BUF accelerated paint support;
- required build integration.

Noctalia owns Wayland scheduling, Vulkan import/cache lifetime, Graphite
dependencies, presentation, input translation, and browser lifecycle.

Keep patches organized by responsibility. A source file should not be modified
by multiple patches in the same area. Temporary diagnostics, experiment
switches, compatibility probes, and application-specific language do not
belong in the shipping CEF stack.

## Validation

The release gate is:

- Vulkan synchronization validation is clean during launch, animation,
  resize, fractional scaling, detach/reopen, and shutdown.
- Continuous Apple Music artwork remains complete and flicker-free.
- Autonomous animation progresses at the compositor refresh rate without
  pointer motion.
- Scrollbar dragging, text input, navigation, audio, authentication, and
  Widevine behavior remain correct.
- Repeated close/reopen and output changes do not strand BeginFrames or Viz
  swaps.
- DMA-BUF imports, exported fence FDs, and memory usage remain bounded.
- Device-loss recovery invalidates old generations and resumes from a fresh
  producer frame.

The bridge's retained counters are operational health signals: accepted,
staged, sampled and discarded frames; import creation/destruction/cache
behavior; active imports; and exported release fences. Detailed per-FD and
per-semaphore lifecycle telemetry is intentionally omitted unless a concrete
fault requires temporary instrumentation.
