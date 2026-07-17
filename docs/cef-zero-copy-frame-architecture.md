# CEF Zero-Copy Frame Architecture

Last updated: 2026-07-15

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
sync FD, and a capture counter. `CefGpuFrameBridge::acceptFrame` validates the
format, modifier, plane layout, dimensions, and Vulkan external-memory support.
It duplicates the borrowed DMA-BUF descriptor only when creating a cached
modifier-backed Vulkan import.

Imports are keyed by stable file identity plus image geometry and layout. The
cache is bounded to five entries, matching Chromium's small capture-buffer
ring. Each entry owns its imported `VkImage`, dedicated `VkDeviceMemory`,
Graphite `BackendTexture`, and `SkImage`.

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

CEF retains the corresponding Viz frame callback until that release fence
signals. Therefore Chromium cannot repaint or recycle a DMA-BUF while Noctalia
is sampling it. A superseded or hidden frame still receives a balanced
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

Graphite recording, texture rebinding, Vulkan queue submission, and CEF frame
acceptance all occur on the main thread. Background workers remain CPU-only.

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

## CEF patch ownership

The maintained CEF patch stack lives in `nucleus-workspace/cef/patches` and
contains only production changes:

- native DMA-BUF accelerated-paint metadata, token-correlated release fences,
  and acknowledged presentation-timed external begin frames;
- Viz native-handle capture-buffer selection;
- Linux Vulkan DMA-BUF/export requirements;
- ANGLE/Chromium pinning to Noctalia's Vulkan device;
- preservation of OSR device scale;
- correct handling of an already-reaped child process;
- transparent hover tracks for Chromium's overlay scrollbars.

CEF's C/C++ translation layer and API hashes are generated from the active
checkout by `nucleus-workspace/cef/build.sh` before compilation. They are never
carried from one CEF branch to another or maintained as source patches.
Noctalia's Meson configure step rejects a nominal CEF SDK that does not expose
the timed BeginFrame, release-fence, capture-token, and producer-fence parts of
this contract.

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
