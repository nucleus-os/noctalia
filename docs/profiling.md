# Profiling Noctalia with Tracy

Noctalia has an opt-in Tracy build for interactive performance diagnosis. The
ordinary debug and release binaries do not compile or link the Tracy client.
Existing aggregate counters and slow-operation logs remain the source for
automated regression gates.

Use the Tracy revision pinned by `nucleus-workspace`. This keeps Noctalia's
client protocol identical to Nucleus's `tracy-capture` and
`tracy-csvexport` tools.

## Configure and build

Starting from the same Nucleus SDK, CEF SDK, and private libc++ paths used by a
normal Noctalia build:

```sh
CC=clang CXX=clang++ \
LD_LIBRARY_PATH="$HOME/.cache/nucleus/noctalia-cpp-deps/clang-21-libcxx/lib" \
meson setup build-tracy-release --buildtype=release \
  -Dtests=disabled \
  -Dcef_sdk_path="$HOME/.cache/nucleus/cef/dist/CEF_VERSION" \
  -Dnucleus_cpp_deps_path="$HOME/.cache/nucleus/noctalia-cpp-deps/clang-21-libcxx" \
  -Dnucleus_render_sdk_path="$HOME/.cache/nucleus/nucleus-native-sdk/render" \
  -Dtracy=enabled \
  -Dtracy_source_path="/path/to/nucleus-workspace/swift-tracy/third-party/tracy"

meson compile -C build-tracy-release noctalia noctalia_cef_helper
```

Tracy is intentionally disabled by default. `tracy_source_path` is rejected
unless it contains the expected pinned client source and headers.

## Capture the Apple Music path

Stop any existing Noctalia process, then run:

```sh
tools/capture-noctalia-tracy.sh --seconds 20
```

The helper asks Nucleus to build or validate its pinned capture tools, launches
the Tracy-enabled Noctalia binary, opens Apple Music through a non-profiled IPC
client, captures the requested interval, shuts Noctalia down cleanly, and
exports zone and plot CSV files. Captures are written below `profiles/`, which
is ignored by Git.

Set `NUCLEUS_WORKSPACE_PATH` if the workspace is not Noctalia's sibling. Use
`--validation` for a correctness-correlated capture. Leave validation disabled
for the fair release performance capture.

External begin frames are mandatory. The Apple Music surface's
`wl_surface.frame` callback is the normal CEF clock. Noctalia supplies CEF an
exact refresh interval and a deadline predicted from the latest realized
presentation phase. CEF's real `BeginFrameAck` completes the one in-flight
request; paint arrival does not. Opportunities that arrive while Chromium is
busy are coalesced to the newest request, and input can mark that request
urgent. Consecutive no-damage acknowledgments enter a slow idle-probe mode.
Scheduling stops while the panel is hidden.

Important zones include:

- CEF message-pump and `OnAcceleratedPaint` work;
- DMA-BUF validation, cached image import, acquire-fence import, and direct
  sampling acquire/release barriers;
- Graphite submission and token-correlated release-fence export. The old
  `CEF record Vulkan copy`, copy-submit, and copy-fence-wait zones no longer
  exist because direct DMA-BUF sampling is the only rendering path;
- Apple Music's frame-ready/update handoff;
- Graphite recorder snapshot, recording insertion, and submission;
- swapchain creation, image acquisition, presentation-fence waits, and queue
  presentation.

CEF-bound pointer, wheel, button, and keyboard events also receive monotonically
increasing interaction sequence numbers. Matching Tracy plots carry each
sequence through input forwarding, CEF pump scheduling/dispatch, accelerated
paint, redraw queuing, Graphite frame start, Vulkan queue presentation, and the
compositor's realized presentation timestamp. Derived plots report
input-to-paint, paint-to-queue-present, input-to-visible, and
queue-present-to-visible latency, plus counts for coalesced input and
superseded correlated paints. Separate
motion, button, wheel, and keyboard plots keep high-rate pointer motion from
obscuring the controls that matter to perceived responsiveness.

Noctalia binds stable `wp_presentation` version 2. A feedback object is created
immediately before each `vkQueuePresentKHR`, so Vulkan WSI's following
`wl_surface.commit` is the exact commit being measured. The callback converts
the compositor clock into the process steady-clock domain and records refresh,
display sequence, presentation flags, delivery delay, and presented/discarded
counts. Pending feedback is explicitly released during surface teardown.

Use the reported refresh interval as the frame budget. For example, the July
15 niri baseline reported exactly 8,333,333 ns for every sample on the 120 Hz
output, not 16.667 ms or 33 ms. It also reported vsync, hardware-clock, and
hardware-completion flags. Native Noctalia repaint pacing remains driven by
`wl_surface.frame`. Presentation feedback is retrospective proof that a commit
became visible, not a guaranteed future deadline; the scheduler projects the
next phase from that proof and rolls an expired target forward by whole refresh
intervals. CEF uses the surface callback as its active clock. Its long watchdog
only diagnoses a missing Chromium acknowledgment and cannot release the
one-in-flight state, while the separate idle probe handles quiet pages without
committing an unchanged buffer continuously.

`Graphite swapchain reason` distinguishes initial creation, size changes, and
acquire/present `OUT_OF_DATE` or `SUBOPTIMAL` results. The apparent second
swapchain during Apple Music startup was measured as a
`vkQueuePresentKHR` `VK_ERROR_OUT_OF_DATE_KHR`, so it is a required WSI
recreation rather than an accidental duplicate resize. A `SUBOPTIMAL` acquire
is still presented once before rebuilding, because it returns a valid acquired
image and a signaled acquire fence.

The first pass traces CPU-side Vulkan calls. Noctalia-owned Vulkan GPU timestamp
zones can be added to the CEF copy command buffer later; Graphite's internal
command buffers are not exposed through its public recording API.
