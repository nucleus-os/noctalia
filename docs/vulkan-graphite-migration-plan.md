# Vulkan 1.4 and Skia Graphite Migration Plan

Last updated: 2026-07-15

> This file retains the broader renderer-migration backlog and historical
> implementation record. Standalone WSI/CEF spikes, gate scripts, and copy-path
> probes described in earlier phases have been removed after real-shell proof.
> The authoritative current CEF contract is
> `docs/cef-zero-copy-frame-architecture.md`.

## Objective

Replace Noctalia's GLES/EGL renderer with a single Vulkan 1.4 renderer built
directly on Skia Graphite's public C++ APIs from the Nucleus render SDK.

The final application will:

- use the same Clang and libc++ ABI contract as Nucleus;
- use one process-wide Vulkan device, Graphite context, and main-thread
  recorder;
- present every native shell surface through Vulkan/Wayland swapchains;
- sample CEF accelerated DMA-BUF frames directly as Graphite images with
  token-correlated acquire/release fences;
- use the shared Nucleus SkParagraph service for text;
- contain no production GLES renderer, CPU CEF fallback, separate VMA
  instance, Volk loader, or Swift Graphite facade.

Linux Wayland, initially under niri, is the supported environment. Vulkan 1.4
is mandatory.

## Fixed architectural decisions

- Nucleus owns the compiler, libc++, Skia, GN, and link-archive contract.
- Noctalia links the Nucleus Skia archives directly and uses public Skia C++
  APIs. It does not link `NucleusSkiaGraphiteBridge`.
- CEF may retain its private Chromium Skia. Noctalia's Nucleus Skia symbols are
  hidden from dynamic symbol interposition at the executable boundary.
- CEF is a required build and runtime dependency and owns the process allocator
  boundary. Nucleus Skia is built with `skia_use_partition_alloc=false`.
- Vulkan is linked directly through `libvulkan`. Volk is not used.
- Skia's `VulkanAMDMemoryAllocator` is the sole allocator for Graphite-owned
  resources.
- CEF's bounded DMA-BUF import cache uses explicit, dedicated Vulkan imports;
  there is no persistent destination image or full-frame copy.
- CEF DMA-BUF descriptors are borrowed callback data. Their FDs are never
  retained after the accelerated-paint callback returns.
- The codec-enabled CEF SDK, Graphite, and Vulkan external-memory support are
  unconditional requirements; there is no CEF-disabled build mode.
- There is no runtime GLES or CPU-rendering fallback in the final product.

## Current status

### Implemented foundation

- Meson requires the Nucleus Clang/libc++ toolchain and rejects GCC or
  libstdc++.
- Meson requires and validates `nucleus_render_sdk_path`.
- A private libc++ dependency bootstrap exists for sdbus-c++ 2.2.1,
  libqalculate 5.9.0, toml++ 3.4.0, libc++, libc++abi, and libunwind.
- Noctalia links the private sdbus-c++ and libqalculate builds rather than the
  system libstdc++ variants.
- The private runtime is installed below Noctalia's library directory with a
  relative installed RUNPATH.
- An ELF/ABI contract checker verifies the intended libc++ dependencies and
  rejects a direct libstdc++ dependency.
- A standalone Vulkan/Graphite Wayland WSI proof exists.
- Initial Graphite WSI rendering and Vulkan validation have passed.
- A standalone CEF/Graphite interop spike, CEF helper, and allocator-boundary
  checks exist.
- A dedicated `CefGpuFrameBridge` implements a bounded DMA-BUF import cache and
  the persistent Vulkan destination-image design.
- Vulkan result fault injection exists. Injector callbacks are invoked outside
  its mutex so one-shot callbacks can safely clear themselves.
- `TextureId` is an opaque 64-bit slot/generation handle.
- Texture generations are allocated monotonically for the lifetime of the
  process-wide `GraphicsDevice`. They do not wrap; exhaustion fails instead of
  allowing a stale handle to alias a new texture.
- Focused texture-handle and Vulkan-result tests pass.
- The production Noctalia executable builds, passes the libc++ ABI contract,
  and requires a validated codec-enabled CEF SDK at configure time.
- Production CEF shutdown now pumps the CEF message loop while waiting for
  `OnBeforeClose` instead of immediately abandoning browser teardown.
- A lifecycle gate script enables Vulkan validation and checks accelerated
  rendering, browser reopen, absence of CPU painting, and clean shutdown.

### Latest gate results

- The patched CEF m150 build completed and produced the codec-enabled minimal
  distribution and tarball.
- The build includes a Chromium process-reaping patch that treats an already
  reaped child (`ECHILD`) as terminated while preserving normal termination
  semantics.
- The refreshed private dependencies, Noctalia executable, CEF helper, and
  Graphite spike build successfully against the completed distribution.
- The libc++ ABI and CEF allocator/ELF contracts pass.
- The validation-enabled lifecycle gate passes with accelerated frames only,
  resize, 1.5x fractional scaling, browser close/reopen, and clean shutdown.
- The lifecycle run reports zero Vulkan validation errors, zero validation
  warnings, zero CPU paints, and no CEF frame-copy failures.
- A 20-second Apple Music run passes at 1.25x scale with 108 accelerated frames,
  nontransparent Vulkan/Graphite output, AAC/MSE support, and clean validation.
- Widevine H.264 capability passes when the installed Google Chrome CDM is
  supplied to the spike.
- Deterministic AAC playback advances successfully while rendering accelerated
  frames with clean validation.
- Widevine protected playback succeeds with the installed CDM and Shaka's
  encrypted Sintel test stream; playback advanced beyond 23 seconds.
- A five-cycle sustained resize/close/reopen run copied 2,643 accelerated
  frames. All transient imports and acquire semaphores were destroyed, all
  duplicated FDs were transferred exactly once, warmed-up process FD growth
  stabilized at one descriptor, and Vulkan validation remained clean.
- The standalone CEF/Graphite interop gate is closed. Production integration
  and longer release-candidate soak testing remain.
- Production now constructs CEF before its application-owned `GraphicsDevice`,
  selects the compositor-presenting RTX 4070 Ti, initializes native Graphite,
  and publishes the selected Vulkan UUID and DRM render node to CEF children.
- A validation-enabled production startup and Apple Music panel run succeeded:
  CEF created the browser, loaded Apple Music with HTTP 200, and delivered the
  first accelerated DMA-BUF frame at the panel's 1.5x fractional scale. Vulkan
  validation reported no VUID errors.
- `setAcceleratedEnabled()` now permits the renderer capability probe after
  process-wide `CefInitialize`; the contract freezes only after browser
  creation. This preserves allocator-first startup without suppressing the
  required accelerated browser path.
- The production Apple Music panel is now a working Graphite/Vulkan vertical
  slice. It uses its own Graphite render context and Vulkan Wayland swapchain,
  while the rest of the shell continues to use the existing GLES renderer.
- Production `OnAcceleratedPaint` frames are copied through
  `CefGpuFrameBridge` into the persistent Vulkan texture and drawn by Graphite;
  there is no CPU paint or GLES CEF fallback.
- Apple Music has been verified visually and audibly, including navigation,
  pointer and keyboard input, continuous accelerated-frame repainting, AAC
  playback, and authentication across clean restarts. CEF now sets both
  `root_cache_path` and `cache_path` to the existing Noctalia CEF directory,
  activating the persistent global request context without relocating its
  `Default` profile. A validation restart preserved the existing Cookies inode
  and loaded Apple Music Home with HTTP 200.
- CEF cursor-shape changes now refresh the active input area instead of
  competing with the compositor surface underneath the panel.
- Pointer release no longer steals focus from CEF text fields.
- Closing the panel hides the persistent browser. Reopening it no longer
  forces navigation to Apple Music Home, so in-session page and playback state
  are retained.
- Shell shutdown restores the cooperative `SIGTERM`/`SIGINT` handlers after
  `CefInitialize`, allowing the browser to reach `OnBeforeClose` before
  Graphite/Vulkan teardown.
- The release baseline reduced measured Apple Music panel render work from
  approximately 3.17 ms per frame to 1.11 ms per frame. Interaction still
  feels slower than desired, so frame-arrival, copy-fence, redraw, and present
  timing remain an active performance gate.
- Noctalia now has an opt-in release-mode Tracy client using Nucleus's pinned
  Tracy source and capture/export tools. The first 10-second Apple Music smoke
  capture recorded 6,560 zones over 235 accelerated CEF frames. It measured
  means of approximately 0.76 ms for the complete CEF frame bridge, 0.32 ms
  each for DMA-BUF import and the mandatory copy-fence wait, 0.47 ms for
  Graphite frame completion, and 0.34 ms inside `vkQueuePresentKHR`.
- A separate validation-enabled Tracy capture loaded Apple Music, produced the
  expected CEF/Graphite zones and plots, shut down cleanly, and reported no
  Vulkan validation messages. This confirms the tracing path works in both
  correctness and fair-performance configurations.
- End-to-end Tracy interaction correlation is implemented. CEF-bound input is
  assigned a sequence carried through input forwarding, message-pump
  scheduling/dispatch, accelerated paint, redraw queuing, Graphite frame start,
  and presentation. A 15-second pointer/scroll smoke run matched 269 correlated
  accelerated paints to 269 presentations with no sequence-order mismatch. Its
  median input-to-present latency was 2.85 ms; p95 was 14.30 ms and p99 was
  35.59 ms.
- A targeted 45-second release capture separated 12,929 inputs into 11,740
  motion, 26 button, 1,149 wheel, and 14 keyboard events. It correlated 609
  accelerated paints with 607 presentations; two paints were superseded before
  presentation. Button, wheel, and keyboard input-to-present p50 values were
  11.01 ms, 5.34 ms, and 23.67 ms respectively, versus 3.26 ms for motion.
  The button and keyboard samples are intentionally treated as directional
  because only five and six of those inputs produced independently correlated
  paints.
- The same capture localizes the slow interaction samples before accelerated
  paint. The CEF Vulkan bridge was 2.34 ms at p95 and 4.19 ms worst-case;
  `vkQueuePresentKHR` was 4.24 ms at p95 and 6.31 ms worst-case. Complete
  accelerated-paint-to-present latency was 1.35 ms at p50 and 7.07 ms at p95.
  No CEF copy or queue-present call exceeded the 8.333 ms frame interval of the
  active 120 Hz output. The next performance investigation should therefore
  focus on Chromium input/raster scheduling and Noctalia main-loop dispatch
  before `OnAcceleratedPaint`, while preserving the post-paint measurements as
  regression gates.
- Normal surface pacing remains driven by Wayland frame callbacks. The
  write-blocked Wayland flush anti-spin fallback no longer assumes a 60 Hz
  display: its minimum wait was reduced from 16 ms to 8 ms so that rare socket
  backpressure cannot itself consume almost two 120 Hz intervals.
- CEF's internal OSR frame-rate cap is now 120 rather than 60. This is a maximum
  damage-production cadence, not a request to repaint unchanged content at 120
  FPS. A 45-second release capture produced 1,896 accelerated frames while the
  animated site was active; a categorized input comparison still needs a
  deliberate click/scroll/type sample.
- `CefGpuFrameBridge` now retains a five-entry LRU of imported modifier-backed
  images keyed by DMA-BUF device/inode and the full image-layout contract. It
  never retains CEF's borrowed descriptors; Vulkan owns a duplicate consumed by
  the dedicated import allocation. Each callback still imports its new sync FD,
  acquires the image from `FOREIGN`, copies into the Noctalia-owned destination,
  releases ownership, and waits for copy completion before returning.
- The bridge also reuses one Vulkan binary semaphore with temporary sync-FD
  payload imports instead of creating and destroying a semaphore per frame.
  The synchronous copy fence guarantees the temporary wait has completed before
  the semaphore is imported again.
- The validation lifecycle/resize/reopen gate copied 25 frames with 22 import
  cache hits and 3 misses, zero evictions, zero validation errors or warnings,
  zero copy failures, and no FD growth after warmup. A production validation run
  recorded 291 hits and 3 misses with no VUID, CPU-paint, copy, or shutdown
  error.
- In comparable release Tracy captures at the same panel geometry, the cache
  reduced complete bridge mean time from 0.842 ms to 0.326 ms and p95 from
  1.790 ms to 0.664 ms. Only 8 of 586 frames created an import in the cached
  run, versus every frame before caching. The mandatory destination copy remains
  intact; this result does not claim zero-copy behavior.
- The deliberate 60-second 120 Hz interaction capture recorded 13,141 inputs,
  997 correlated accelerated paints, and 967 correlated presentations. Wheel
  input-to-present improved from 5.34 to 2.69 ms at p50 and from 14.42 to
  11.26 ms at p95 across 170 new samples. Motion p95 improved from 17.77 to
  15.17 ms, while its p50 moved slightly from 3.26 to 3.59 ms.
- Button input-to-present p50 improved from 11.01 to 7.86 ms, but both captures
  contain only five correlated button paints. Keyboard p50 was effectively
  unchanged at 23.14 versus 23.67 ms with only seven versus six samples. Treat
  both categories as directional rather than statistically conclusive.
- The same capture reduced input-to-message-pump-dispatch p95 from 108.31 to
  44.27 ms. Bridge work fell from 1.037 to 0.483 ms mean and from 2.340 to
  1.331 ms p95 despite accelerated-frame production increasing from 892 frames
  in 45 seconds to 2,357 frames in 60 seconds. The cache served 2,352 hits with
  5 misses and no evictions. Thirty correlated paints were superseded before
  presentation, consistent with the higher cadence favoring the newest frame.
- Targeted pre-paint probes now reset at each input and stop attribution when
  its correlated accelerated paint arrives; the earlier pump-dispatch metric
  incorrectly continued attributing periodic pumps after paint. In a 60-second
  capture, input forwarding was below 0.001 ms at p95, accepted pump request to
  dispatch was 0.122 ms at p95, and poll-deadline lateness was normally 0–1 ms.
- Accelerated paint arrived 0.033 ms after the final CEF pump dispatch at p50
  and 0.060 ms at p95. However, keyboard paint arrived 21.13 ms after its first
  pump dispatch at p50 across 11 samples. This localizes the persistent keyboard
  delay inside Chromium's multi-pump input/scheduling/raster work rather than
  Noctalia input forwarding, poll wakeup, Vulkan import/copy, or presentation.
- The first opt-in `NOCTALIA_CEF_EXTERNAL_BEGIN_FRAME=1` experiment used a
  free-running 8/8/9 ms cadence (120 requests/second), immediate coalesced
  begin frames after button and keyboard delivery, and stopped while hidden.
  Its independent timer was rejected; it is not the presentation-aware policy
  that is now the production default.
- The external-scheduler validation capture loaded Apple Music at HTTP 200,
  sustained approximately 118 begin-frame requests/second over its active
  interval, and reported no VUIDs, CPU paints, copy failures, or shutdown
  errors.
- That initial latency A/B rejected free-running immediate external begin
  frames as an optimization.
  Keyboard input-to-paint p50 increased from 21.41 ms internally to 25.76 ms
  externally across 101 external-scheduler samples. Immediate begin-frame to
  paint itself was 24.72 ms at p50, keyboard frames consumed a median of eight
  pump dispatches, and final pump-to-paint remained only 0.038 ms. Chromium's
  multi-stage input/raster pipeline therefore persists across both scheduler
  modes. It motivated scheduling from realized compositor phase rather than
  merely matching the output's numeric refresh rate.
- Stable Wayland presentation-time v2 is now bound process-wide. Each Graphite
  surface requests feedback immediately before `vkQueuePresentKHR`, associates
  the returned realized timestamp with its interaction trace, converts the
  compositor clock into the process steady-clock domain, and records refresh,
  sequence, flags, delivery delay, presented/discarded counts, and pending
  feedback lifetime.
- A fair release capture reported 382 of 383 submitted frames as presented
  before shutdown, zero pending feedback after teardown, an exact clock-domain
  conversion for every callback, and a constant 8,333,333 ns refresh interval.
  Niri reported flags `0x7`: vsync, hardware clock, and hardware completion.
- A validation-enabled follow-up reported 504 presented frames with the same
  exact 8,333,333 ns interval and no Vulkan validation messages. It recorded
  525 CEF frame-ready requests and 505 Graphite presentations, confirming that
  the existing redraw queue coalesces newer browser frames rather than issuing
  one surface commit for every callback.
- The final 20-second non-validation baseline reported 385 compositor-presented
  frames, 388 Graphite submissions, and 353 CEF frame-ready callbacks. The CEF
  bridge mean was 0.323 ms, including a 0.285 ms mandatory copy-fence wait;
  `vkQueuePresentKHR` averaged 0.628 ms. These remain well below the measured
  8.333 ms display interval, and Apple Music loaded with HTTP 200 and shut down
  cleanly.
- Swapchain creation now records its cause. The second same-sized swapchain on
  Apple Music startup is caused by `vkQueuePresentKHR` returning
  `VK_ERROR_OUT_OF_DATE_KHR`, not by a redundant `resize()` call; recreating it
  is required. The `VK_SUBOPTIMAL_KHR` acquire path was corrected to present
  its valid acquired image once before rebuilding, avoiding abandonment of the
  image and reuse of a signaled acquire fence.
- Compositor-paced OSR scheduling is wired from the Apple Music panel's
  `wl_surface.frame` callback into `CefService`. A painted frame commits through
  Graphite and the compositor's next frame opportunity advances Chromium.
  Latency-sensitive input wakes immediately, duplicate requests coalesce, and
  a bounded watchdog handles CEF's unacknowledged no-damage case before backing
  off to slow idle probes. The watchdog is not the active frame clock. CEF's
  internal clock and the former predictive presentation timer were removed.
- The July 15 validation gate reported a constant 8,333,333 ns interval with
  exact clock conversion, loaded the authenticated Apple Music home page at
  HTTP 200, and produced no Vulkan VUID or synchronization diagnostics. After
  adding the activity burst, first accelerated paint improved from about
  3.05 seconds to 0.72 seconds after browser creation. The 20-second trace
  copied 319 CEF frames, received 327 presentation reports, and shut down
  cleanly; the learned CEF paint estimate was 5.22 ms at p50.

### Important status caveat

The general production Noctalia renderer is still GLES/EGL. Apple Music is the
first production Graphite/Vulkan surface, but `GraphiteRenderBackend` currently
implements only the subset needed for that vertical slice. Specialized SkSL
effects, offscreen rendering, most native UI paths, text migration, and the
final GLES removal remain incomplete.

### Immediate next work

1. Run a deliberate release-mode click, scroll, and typing capture so the new
   compositor callbacks produce statistically useful input-to-visible and
   queue-present-to-visible percentiles for each input category.
2. Validate presentation-aware scheduling at 60 Hz, across an output move, and
   under VRR if available; compare it with the internal-clock escape hatch at
   identical panel geometry and interaction workload.
3. Run a longer release-mode Apple Music soak covering navigation, input,
   playback, close/reopen, resize, fractional scale, FD counts, GPU allocation
   counts, and Vulkan validation.
4. Continue the native renderer port by completing Graphite primitives,
   resource lifetime handling, and offscreen surfaces.

The Tracy build is opt-in and uses the source revision and capture/export tools
pinned by `nucleus-workspace`. Normal builds remain Tracy-free, and aggregate
counters remain in place for automated performance gates. See
[`profiling.md`](profiling.md) for the build and capture workflow.

## Ordered implementation plan

### Gate 1: finish and validate the patched CEF build — complete

This gate blocks the full renderer migration.

1. Finish and package the patched CEF build.
2. Point Noctalia's CEF spike targets at the new package.
3. Rebuild the private libqalculate package with compiled definitions enabled.
4. Relink Noctalia and rerun both the CEF ELF contract and libc++ ABI contract.
5. Run the lifecycle gate with Vulkan validation enabled.
6. Run the full CEF/Graphite pixel-copy gate against ordinary web content and
   Apple Music.
7. Exercise repeated frames, resize, fractional scale, close/reopen, and
   sustained playback.
8. Record FD and Vulkan allocation counts before, during, and after the run.

The gate passes only when:

- accelerated DMA-BUF frames are nontransparent and correctly oriented;
- `OnPaint` is never used;
- Vulkan validation reports no ownership, layout, external-memory,
  synchronization, or lifetime errors;
- browser close and reopen complete normally;
- shutdown produces no `waitpid(...): ECHILD` or `OnBeforeClose` timeout;
- FD and allocation counts remain stable;
- AAC and Widevine playback remain functional.

If the gate fails, correct the import, allocator, or lifecycle contract. Do not
add a CPU or GLES fallback.

### Phase 1: finish the reproducible build contract

- Make the private dependency bootstrap reproducible from an empty cache.
- Validate archive checksums, compiler identity, libc++ ABI identity, Skia
  commit, GN arguments hash, target triple, and archive inventory.
- Verify debug and release configurations with the required codec-enabled CEF SDK.
- Verify installed binaries from a staged prefix with `readelf` and `ldd`.
- Ensure Noctalia, sdbus-c++, and libqalculate do not load system libstdc++.
- Add SDK and private dependency bootstrapping to CI.
- Update Nix, Guix, distro packaging, developer documentation, and credits.
- Treat SDK and dependency cache directories strictly as generated output.

Exit criterion: all supported build configurations produce an ABI-compatible,
relocatable installation from a clean environment.

### Phase 2: adopt `GraphicsDevice` in production — partially complete

- Create one application-owned `GraphicsDevice` during production startup.
- Require Vulkan 1.4 and the Graphite feature set.
- Select a queue family that supports both graphics and presentation to the
  active Wayland display.
- Ensure Noctalia and CEF use the compositor-presenting physical GPU.
- Require `VK_KHR_swapchain`, `VK_KHR_swapchain_maintenance1`, and
  premultiplied composite alpha.
- Require external-memory FD, DMA-BUF modifier, and foreign queue-family
  support for the mandatory CEF bridge.
- Create one native Graphite context and one main-thread recorder.
- Emit clear startup diagnostics when the device contract cannot be satisfied.
- Replace `GlSharedContext`, `makeCurrent`, GL framebuffer binding, and GL reset
  APIs with explicit surface/offscreen frame scopes.

Exit criterion: production startup owns one validated Vulkan/Graphite device
and does not initialize a production EGL context.

### Phase 3: complete Vulkan/Wayland presentation — vertical slice complete

Give every Wayland surface:

- a `VkSurfaceKHR` and swapchain;
- two frame slots with command resources, acquire semaphores, and completion
  fences;
- per-swapchain-image present semaphores and presentation fences;
- retired swapchain generations retained until their fences signal;
- Graphite `SkSurface` wrappers for acquired swapchain images.

Presentation behavior must:

- prefer `B8G8R8A8_UNORM`, then `R8G8B8A8_UNORM`, with
  `SRGB_NONLINEAR` presentation;
- submit Graphite work with the acquire semaphore and signal the present
  semaphore;
- transition images to `PRESENT_SRC_KHR` before presentation;
- use FIFO presentation and Wayland frame callbacks for repaint throttling;
- handle zero extent, resize, `OUT_OF_DATE`, `SUBOPTIMAL`, surface loss,
  hotplug, and destruction;
- avoid steady-state `vkQueueWaitIdle` and `vkDeviceWaitIdle`.

Exit criterion: representative production surfaces present Graphite frames
through Vulkan with clean validation and stable frame pacing.

### Phase 4: implement device-loss recovery

- Replace GL reset classifications with `RenderFrameStatus` and
  `RenderDeviceStatus`.
- Detect and propagate `VK_ERROR_DEVICE_LOST`.
- Stop further submissions and perform one complete GPU rebuild.
- Recreate Graphite, the recorder, and all swapchains.
- Invalidate all old texture handles using the device-lifetime generation
  allocator.
- Reload CPU-backed assets and rebuild offscreen caches.
- Invalidate the CEF destination image and request a fresh accelerated frame.
- Add fault-injected unit and integration tests for acquire, submit, present,
  and device loss.

Exit criterion: an injected device loss either recovers the complete renderer
once or terminates with a clear error; no stale GPU object remains usable.

### Phase 5: move the CEF bridge into the production panel — functional, performance gate open

- Replace generic retained DMA-BUF imports with
  `CefGpuFrameBridge::copyFrame(BorrowedDmabufFrame)`.
- Validate fourcc, modifier, plane count, offsets, pitches, dimensions, and
  external-memory capabilities in `OnAcceleratedPaint`.
- Duplicate borrowed FDs only for immediate Vulkan consumption.
- On a cache miss, create a modifier-backed `VkImage` with explicit plane
  layouts and import a duplicate FD through dedicated `VkDeviceMemory`.
- Retain at most five exact-contract imports in an LRU keyed by DMA-BUF
  device/inode, dimensions, format, modifier, stride, and offset. Evict on
  incompatible reuse, resize churn, or capacity pressure.
- Acquire the source image from `VK_QUEUE_FAMILY_FOREIGN_EXT`.
- Copy on the Graphite graphics queue into one persistent Noctalia-owned image.
- Transition the destination to shader-read layout and wait for the mandatory
  copy fence before returning from the callback.
- Retain cached imported images until eviction, invalidation, or bridge teardown;
  destroy all imports only after their synchronous copy use has completed.
- Wrap the persistent image directly as a Graphite image.
- Make `OnPaint` an explicit unsupported-path panel error and retain no CPU
  frame.
- Handle resize, panel reopen, and device recovery.

Exit criterion: the real Apple Music panel uses only accelerated CEF frames and
Graphite drawing, with stable resources and clean validation.

### CEF cache, refresh cadence, and external begin-frame scheduling

#### Current behavior and terminology

Noctalia sets `CefSettings.root_cache_path` and `CefSettings.cache_path` to the
same existing CEF directory. These fields are not interchangeable: the root
path stores installation-wide data and establishes the profile-directory
boundary, while the non-empty cache path activates the persistent global
request context. Reusing the existing root avoided a profile relocation and
preserved the existing Apple Music data. Continue to follow the pinned CEF m150
[`CefSettings` contract](https://github.com/chromiumembedded/cef/blob/b88780525f76c1c1d67d30b87e3b16bb37955175/include/internal/cef_types.h)
rather than inferring profile behavior from Chromium's generated command line.

The browser sets `CefBrowserSettings.windowless_frame_rate` to 120. This is
CEF's maximum internally generated offscreen frame cadence, not the Wayland
output refresh rate. The active display is 120 Hz, with a nominal 8.333 ms
interval, so the former free-running 60 Hz CEF clock could add almost one
display interval before Chromium began work and could drift out of phase with
the compositor. CEF m150 accepts any positive frame rate and does not clamp the
API to 60.

`wl_surface.frame` and presentation feedback have different meanings:

- a frame callback throttles the client and indicates that the compositor is
  ready for another surface update;
- it does not prove that the previous image became visible and does not expose
  a precise future deadline;
- `wp_presentation_feedback` is requested for a particular
  `wl_surface.commit` and later reports either that it was presented or that it
  was discarded;
- a presented result includes the realized presentation timestamp, synchronized
  output, refresh interval in nanoseconds, display sequence, and flags describing
  vblank synchronization and timestamp quality.

These semantics come from the stable Wayland
[`presentation-time`](https://wayland.app/protocols/presentation-time)
protocol; presentation feedback is associated with an individual surface
commit and is destroyed after reporting `presented` or `discarded`.

Noctalia binds `wp_presentation` version 2 and requests one feedback object
immediately before every Graphite `vkQueuePresentKHR`. Vulkan's Wayland WSI
performs the associated `wl_surface.commit`; the resulting callback therefore
extends Tracy's former queue-present milestone to the timestamp at which the
pixels actually became visible. Feedback remains retrospective rather than a
guaranteed future deadline. The Apple Music surface predicts its next CEF
target from the last realized presentation plus the reported refresh interval;
under VRR the interval may vary or be unavailable, so every nonzero feedback
sample replaces the previous cadence instead of assuming 60 or 120 Hz.

The per-surface timing state contains:

- the synchronized output and its nominal mode refresh;
- the latest compositor-reported refresh interval;
- the latest realized presentation timestamp and sequence;
- presented-versus-discarded commit counts;
- rolling CEF begin-to-paint, Vulkan-copy, Graphite, and compositor-latency
  estimates;
- a predicted target and render-start deadline with a safety margin.

A conceptual prediction is:

```text
next target = last realized presentation + reported refresh interval

render deadline = next target
                - estimated CEF render time
                - Vulkan copy time
                - Graphite/presentation time
                - safety margin
```

#### External CEF begin frames

With CEF's normal windowless scheduler, Chromium generates begin frames from an
independent clock. A begin frame advances `requestAnimationFrame`, CSS
animations, style/layout, paint, rasterization, and compositing before an
accelerated frame can reach `OnAcceleratedPaint`. Equal numeric refresh rates
are insufficient if the CEF and compositor clocks have different phases: a web
frame can begin just after one CEF tick and finish just after a compositor
deadline, losing two opportunities.

Setting `CefWindowInfo.external_begin_frame_enabled=1` transfers control of
that clock to Noctalia through `CefBrowserHost::SendExternalBeginFrame()`.
This allows ongoing Chromium work to follow Wayland's actual cadence instead
of an unrelated timer. The intended pipeline is:

```text
Wayland frame opportunity / predicted presentation phase
    -> issue one external CEF begin frame
    -> Chromium runs JS, layout, raster, and compositing
    -> OnAcceleratedPaint supplies a borrowed DMA-BUF
    -> copy into the persistent Noctalia Vulkan image
    -> mark the Graphite surface dirty
    -> render and commit the newest image
    -> presentation feedback reports when it became visible
```

Latency-sensitive input is special: a pointer button or key event should issue
an immediate begin frame rather than waiting for the next Wayland callback.
Subsequent requests must be coalesced while a begin frame or accelerated paint
is in flight. If Chromium misses a target, do not queue historical web frames;
present the newest completed frame at the next opportunity.

Do not send external begin frames unconditionally at 120 Hz. Even without new
pixels, a begin frame can advance JavaScript animation callbacks, style/layout,
and compositor scheduling. Apple Music may contain progress indicators,
animated artwork, and persistent `requestAnimationFrame` handlers, so a blind
120 Hz loop could double site-side work and reduce responsiveness.

Use an adaptive policy:

- issue immediately after visible input, resize, invalidation, or panel attach;
- allow only one outstanding begin-frame request and coalesce duplicate input;
- continue at the synchronized output cadence while accelerated paints keep
  arriving or known visual animation is active;
- stop or progressively back off after multiple begin frames produce no damage;
- restart promptly when new input or invalidation arrives;
- stop completely while the browser is hidden with `WasHidden(true)`;
- treat audio playback alone as insufficient reason to raster at 120 Hz.

There is a feedback-loop edge case: Wayland frame callbacks normally follow a
surface commit, but CEF may produce no new image and therefore give Noctalia no
reason to commit. The implementation needs a bounded adaptive timer or explicit
active-animation state to restart or sustain CEF when necessary without
committing an unchanged buffer forever.

#### Backend constraints during this work

- Keep Ozone Wayland and ANGLE Vulkan as the production baseline because that
  is the validated accelerated DMA-BUF export path on the target NVIDIA/niri
  system.
- Keep Chromium native Vulkan disabled; its Wayland path has already produced
  unusable external images and Chromium reports the combination as unsupported.
- Keep Chromium on Ganesh. Chromium m150 disables Graphite by default and
  treats desktop Linux Graphite as unsupported; forcing Graphite would add Dawn
  and place the external-image contract on an unvalidated path.
- Windowless CEF uses the Alloy runtime on Linux; Chrome runtime is not an
  alternative for this OSR integration.
- Treat Ozone X11 plus ANGLE Vulkan and forced GPU rasterization as isolated A/B
  experiments, not default switches. Re-run pixel, input, playback, validation,
  FD, and allocation gates for each experiment.
- Do not use broad flags such as `--disable-frame-rate-limit`,
  `--disable-gpu-vsync`, `--ignore-gpu-blocklist`, or global background
  throttling disables without a measured, narrowly reproduced reason.

#### Rollout and acceptance sequence

1. Persistent profile/cache: implemented; existing profile reuse and clean
   validation restart pass. Reconfirm Apple Music authentication visually after
   rollout.
2. Internal CEF frame rate 60 to 120: implemented; accelerated rendering and
   validation pass. Repeat the categorized Tracy interaction capture at the
   same panel geometry and scale.
3. Presentation feedback and end-to-end visible-latency measurements:
   implemented and validation-clean.
4. Per-surface cadence tracking without a hard-coded 60/120 Hz output:
   implemented; fixed 120 Hz is validated and multi-rate/VRR remains a live
   acceptance test.
5. Presentation-aware external begin-frame scheduling: implemented as the
   default, with the internal scheduler retained only as a diagnostic A/B.
6. Immediate input wakeup, single-in-flight coalescing, activity bursts,
   adaptive no-damage backoff, and hidden-panel suspension: implemented.
7. Keep the change only if deliberate interaction and soak captures lower or
   preserve input-to-visible tails without raising
   idle CPU/GPU use, breaking AAC/Widevine playback, destabilizing DMA-BUF
   lifetime, or introducing Vulkan validation errors.

### Apple Music transparent-background integration — planned after CEF performance stabilization

Make Apple Music visually participate in Noctalia's panel theme instead of
painting a full opaque webpage background.

Implementation requirements:

- Create the windowless CEF browser with a transparent background color.
- Inject a versioned Noctalia stylesheet after main-frame load and after
  relevant single-page navigations.
- Make the document root and Apple Music's large structural background layers
  transparent or theme-tinted while leaving artwork, controls, menus, text
  contrast surfaces, and intentional cards legible.
- Prefer resilient root/semantic selectors and a small set of guarded Apple
  Music selectors. Log when expected targets disappear so upstream site
  changes fail visibly rather than silently degrading the theme.
- Preserve premultiplied alpha through CEF's DMA-BUF, the transient Vulkan
  import, the persistent destination image, Graphite sampling, and swapchain
  composition.
- Draw Noctalia's configured panel color and backdrop blur behind the CEF node.
  CSS transparency alone is insufficient to expose compositor blur.
- Expose the injected tint/opacity through Noctalia theme values instead of
  hard-coding a second color system.
- Reapply the style without forcing a page reload and without interfering with
  Apple Music authentication, navigation, media playback, input, or CSP.

Verification:

- Compare opaque, translucent, and fully transparent root backgrounds.
- Test light/dark theme changes, modal layers, menus, lyrics, search, library,
  album and artist pages, MiniPlayer/full player states, and loss of artwork.
- Verify correct premultiplied edges with Vulkan validation enabled.
- Confirm the effect does not add per-frame JavaScript/style work or regress
  frame pacing.
- Treat Apple Music DOM/CSS churn as an ongoing compatibility risk and keep the
  injected stylesheet isolated and easy to update or disable diagnostically.

Exit criterion: Noctalia's panel color and blur show through the intended page
regions with readable controls, correct alpha, stable performance, and no
change to Apple Music behavior.

### Phase 6: implement the production `GraphiteRenderBackend`

Preserve the scene-node and high-level `Renderer` vocabulary while replacing
the implementation.

Use native `SkCanvas` operations for:

- images, tinting, and alpha masks;
- solid and rounded rectangles;
- clipping, transforms, and top-left coordinate matrices;
- linear gradients;
- Gaussian blur and saturation;
- ordinary borders and shadows;
- explicit disabled, straight-alpha, and premultiplied blend behavior.

Port specialized behavior to cached `SkRuntimeEffect` SkSL:

- concave and inverted advanced rectangles;
- screen corners;
- spinner and countdown ring;
- graph and audio spectrum;
- fancy audio visualizer;
- weather and effect nodes;
- wallpaper transitions with multiple child shaders.

Compile runtime effects once during startup. A compilation failure is a named
startup error, not permission to change or omit the visual behavior.

Exit criterion: every renderer primitive and specialized effect has a Graphite
implementation and a passing golden comparison.

### Phase 7: replace framebuffer and offscreen rendering

- Replace GL framebuffers with Graphite offscreen `SkSurface` instances.
- Replace framebuffer caches with surface snapshots.
- Port `CachedLayer`, blur caches, wallpapers, backdrops, and thumbnails.
- Track resource retirement through Graphite submission completion.
- Keep asynchronous workers CPU-only.
- Decode images off-thread and create/upload Graphite images on the main
  recorder.
- Use mipmapped Graphite images for static assets unless mipmaps are disabled.
- Never mipmap CEF frames or frequently updated graph/audio textures.

Exit criterion: all caches and offscreen effects are recorder operations and no
renderer path requires an auxiliary GL context.

### Phase 8: migrate image and SVG handling

- Use SkSVG for SVG rasterization.
- Remove Cairo/librsvg from image rendering after parity.
- Verify orientation, premultiplied alpha, color space, scaling, and mipmap
  behavior.
- Add raster and SVG golden tests.

Exit criterion: static and dynamic image paths render through Skia/Graphite with
no Cairo/librsvg renderer dependency.

### Phase 9: extend the shared Nucleus SkParagraph service

Extend Nucleus rather than creating a Noctalia-only text engine.

API work:

- `EllipsisMode::{None, Start, Middle, End}`;
- automatic, LTR, and RTL paragraph direction;
- structured `TextRun` styling;
- underline and strike-through decorations;
- arbitrary font weight, italic, monospace family selection, and color;
- baselines, ink bounds, cap height, cursor positions, line metrics, hit
  testing, and selection rectangles;
- UTF-8/UTF-16 offset mapping.

Implementation work:

- retain native SkParagraph tail ellipsis;
- implement start and middle ellipsis using ICU/SkUnicode grapheme boundaries
  and width-based search;
- detect paragraph direction unless explicitly overridden;
- invalidate the shared font collection and affected layout caches when plugin
  fonts change.

Exit criterion: Nucleus exposes every text-layout capability required by
Noctalia with coverage for Latin, CJK, Arabic, bidi, combining marks, emoji,
color fonts, and custom fonts.

### Phase 10: port Noctalia text, markdown, editing, and icons

- Replace `CairoTextRenderer` with cached paragraph handles painted directly
  onto the current Graphite canvas.
- Replace generated Pango markup with structured text runs.
- Replace `CairoGlyphRenderer` with icon-font `SkTypeface`/`SkFont` drawing.
- Let Graphite own glyph atlases.
- Port labels, wrapping, all ellipsis modes, password masks, multiline editing,
  cursor movement, hit testing, and selection rendering.
- Remove Pango, Cairo, pangocairo, direct HarfBuzz, direct FreeType, and
  librsvg renderer dependencies after parity.
- Retain Fontconfig only where family discovery and plugin registration still
  require it.

Exit criterion: every text and icon consumer uses SkParagraph/Skia and passes
the text acceptance matrix.

### Phase 11: cut over all shell surfaces

Move all consumers to the shared Graphite device:

- bars and widgets;
- panels and centered panels;
- menus and popups;
- notifications and OSDs;
- desktop widgets;
- wallpaper and backdrop surfaces;
- dock and window switcher;
- lockscreen and greeter;
- screenshot overlays;
- thumbnail and image caches;
- Apple Music and other CEF surface nodes.

Development may use build-time staging switches. The final production build
contains one renderer and no runtime fallback.

Exit criterion: every visible Noctalia surface is rendered and presented by
Graphite/Vulkan.

### Phase 12: remove GLES/EGL

- Delete `GlesRenderBackend`, `GlesTextureManager`, and GL framebuffer classes.
- Delete GLSL program infrastructure and GL extension probes.
- Delete EGL context, reset, external-texture, and Wayland-EGL code.
- Remove NDC and GL Y-flip logic.
- Remove direct EGL, GLES, and `wayland-egl` link dependencies.
- Remove `[shell].shared_gl_context` through a configuration migration; do not
  retain it as an ignored option.
- Confirm CEF is the only component that may load graphics libraries
  transitively.

Exit criterion: repository and binary inspection find no production GLES/EGL
renderer code or dependency.

### Phase 13: validation, performance, documentation, and release

- Run Graphite goldens for every primitive, effect, cache, image, SVG, and
  transparency path.
- Run the full text matrix.
- Run CEF at animated 60 FPS with navigation, input, resize, fractional scale,
  close/reopen, AAC, and Widevine.
- Validate multiple monitors, output hotplug, suspend/resume, lockscreen,
  wallpaper/backdrop, transparent surfaces, panels, notifications, and Apple
  Music simultaneously under niri.
- Keep Vulkan validation clean across all runs.
- Compare CPU usage, GPU time, frame pacing, memory, and CEF callback latency
  with the captured GLES baseline.
- Permit no persistent per-frame CPU synchronization outside the mandatory CEF
  copy fence.
- Update architecture documentation for Graphite recording, Vulkan
  presentation ownership, CEF callback lifetimes, and device recovery.
- Complete CI and packaging validation from clean environments.

Exit criterion: all final acceptance criteria below pass in a releasable build.

## Test matrix

### ABI and build

- Debug and release.
- Required codec-enabled CEF configuration.
- Clean-cache bootstrap.
- Staged installation and relocation.
- No unintended system libstdc++ dependency.

### Vulkan and resource lifetime

- Device filtering and Wayland-present queue selection.
- Swapchain state transitions and retirement.
- Texture slot/generation behavior.
- Modifier plane construction and FD ownership.
- Resize, zero extent, out-of-date, suboptimal, and surface loss.
- Fault-injected device loss and complete rebuild.
- Sustained FD, image, memory, and fence stability.

### Rendering

- Every native primitive and SkSL effect.
- Premultiplied transparency and blend modes.
- Clipping, transforms, blur, shadows, and saturation.
- Wallpaper transitions and offscreen caches.
- Static mipmaps and dynamic texture updates.
- Raster images and SVG.

### Text

- Latin, CJK, Arabic, and mixed bidi.
- Combining marks, emoji, and color fonts.
- Plugin/custom fonts and cache invalidation.
- Structured markdown styles and decorations.
- Every ellipsis and wrapping mode.
- Baselines, ink bounds, cursor stops, hit testing, and selections.
- Password masks and multiline editing.

### CEF

- Example content and Apple Music.
- Animated content at the active output cadence, including 60 and 120 Hz.
- Navigation, keyboard, pointer, and scrolling input.
- Persistent HTTP cache, cookies, local storage, IndexedDB, service workers,
  preferences, and authentication across clean restarts.
- Wayland presentation timestamps, refresh intervals, output association,
  discarded commits, and measured input-to-visible latency.
- Internal 120 Hz versus adaptive external begin-frame A/B coverage.
- No unnecessary begin-frame loop or elevated idle CPU/GPU usage on unchanged
  content.
- Resize, fractional scaling, and close/reopen.
- AAC and Widevine playback.
- No `OnPaint` use or retained CPU frame.
- Clean Vulkan validation.
- Sustained FD and memory stability.

### Live compositor validation

- Multiple monitors and fractional scaling.
- Output hotplug.
- Suspend and resume.
- Bars, panels, popups, notifications, OSDs, and desktop widgets.
- Wallpaper, backdrop, lockscreen, and transparent surfaces.
- Apple Music active alongside the rest of the shell.

## Principal risks

### CEF external-memory ownership

DMA-BUF modifier layout, foreign queue ownership, and callback FD lifetime must
match the producer contract exactly. This is why the CEF/Graphite gate blocks
the rest of the migration.

### Process-wide ABI and allocator interactions

CEF, Nucleus Skia, libc++, and third-party C++ libraries share a process. The
build manifest, hidden Skia symbols, private dependencies, and ELF checks are
required safeguards, not optional packaging work.

### Vulkan presentation lifetime

Swapchains, wrapped Graphite surfaces, semaphores, fences, and retired
generations must remain alive until the GPU and presentation engine are
finished. Over-synchronizing would hide bugs while harming frame pacing.

### Visual parity

Noctalia contains many specialized shader and offscreen paths. Each must be
ported deliberately and compared against captured output rather than replaced
with an approximate effect.

### Text scope

The text migration includes layout, styling, Unicode segmentation, bidi,
editing geometry, font registration, and icons. It is a shared Nucleus API
project, not a simple renderer substitution.

## Final acceptance criteria

The migration is complete only when:

- Noctalia starts only on a compatible Vulkan 1.4 device;
- the selected device is capable of presenting to the active Wayland
  compositor;
- every native UI path renders through Skia Graphite;
- every Wayland surface presents through Vulkan;
- Apple Music is fully interactive through accelerated CEF rendering;
- AAC and Widevine playback work;
- Vulkan validation is clean;
- FD and GPU allocation counts remain stable;
- device-loss recovery invalidates stale resources and rebuilds the renderer;
- the production binary has no GLES/EGL renderer or CPU CEF fallback;
- the intended Clang/libc++ and Nucleus SDK ABI contracts pass in packaged
  builds.
