# Apple Music CEF Panel Interactive Resize Plan

Last updated: 2026-07-20

Status: design complete; implementation has not started.

## Decision

Add client-side corner resizing to the normal Apple Music panel while keeping
it a Noctalia-owned layer-shell surface.

Do not convert the normal panel into an `xdg_toplevel`. A toplevel would give
niri ownership of interactive resize, but it would also change the panel into
a normal compositor window and reintroduce window rules, dock/task entries,
focus behavior, placement animation, outside-click dismissal, and fullscreen
handoff problems that the layer-shell panel already solves.

No Chromium, CEF, niri, Nucleus render SDK, or Graphite API changes are
required. The existing Noctalia stack already supports every downstream size
transition:

```text
corner drag
  -> PanelManager computes a new logical body rectangle
  -> LayerSurface requests new size/margins
  -> niri configures the layer surface
  -> Surface resizes its Vulkan/Graphite target
  -> PanelManager relays out decoration, blur, input, and content
  -> CefSurfaceNode gives CEF the new logical viewport
  -> CEF publishes a correctly sized accelerated DMA-BUF frame
```

The missing work is resize interaction, geometry ownership, request
coalescing, and persistence—not a new rendering path.

## Product behavior

### When resizing is available

- A panel opened only by hovering the Apple Music topbar item remains a
  provisional preview. It is not resizable and shows no resize cursor.
- Clicking the Apple Music topbar item retains the panel. Its four corners then
  become resize handles.
- A click-opened or keybind-opened retained panel is resizable.
- Fullscreen presentation is not resizable.
- Returning from fullscreen restores the exact retained normal-panel size.
- Closing and reopening the panel restores the last accepted normal size.

The opening gesture remains the authority for panel lifetime. Pressing or
dragging inside a hover preview must not implicitly retain it.

### Interaction

- The four corners use the standard northwest/southeast and
  northeast/southwest resize cursors.
- Pressing a corner starts an implicit Wayland pointer-grab resize transaction.
- Dragging freely changes width and height; Apple Music is responsive and the
  panel does not preserve a fixed aspect ratio.
- The corner opposite the dragged corner stays fixed in output coordinates.
- Releasing the primary button commits and persists the final size.
- `Escape` during a resize cancels the transaction and restores the initial
  geometry without forwarding Escape to CEF.
- CEF must never receive the press, motion, release, or cursor request owned by
  a resize handle.

### Constraints

- Work in logical pixels. Fractional output scale affects buffer size, not the
  persisted panel size.
- Start with a `640x480` logical minimum, then validate Apple Music's smallest
  useful responsive layout before final acceptance.
- Clamp the maximum to the target output's usable bounds, including the
  panel's shadow/safety outsets and normal screen padding.
- Never allow a resize to move the complete panel body off-output.
- A saved size that no longer fits after monitor or scale changes is clamped
  when the panel opens; the saved value itself need not be destructively
  rewritten until the user completes another resize.

## Current implementation

The normal Apple Music panel currently reports a fixed `1120x720` logical
preferred size in:

- `src/shell/apple_music/apple_music_panel.h`

`PanelManager::openPanel()` resolves that body size into a layer-shell surface
with separate outsets for shadow, safety padding, and the provisional hover
corridor. It records:

- the visible body in `m_panelOutputRect`;
- the body offset inside the surface in `m_panelInsetX/Y`;
- the body dimensions in `m_panelVisualWidth/Height`;
- trailing surface outsets in `m_detachedBleedRight/Bottom`;
- the current layer-shell anchors and margins.

The required runtime Wayland operations already exist:

- `LayerSurface::requestSize(width, height)`;
- `LayerSurface::setMargins(top, right, bottom, left)`;
- the normal configure callback and render-target resize transaction.

The CEF path already performs the right logical handoff. During layout,
`CefSurfaceNode` calls `CefService::resize()`, which compares the dimensions,
calls `CefBrowserHost::WasResized()`, and requests an external BeginFrame.
Until CEF publishes the first complete frame at the new size, the node keeps
the previous complete texture visible and scales it to the current scene
rectangle. No CPU frame and no non-Graphite fallback are needed.

## Geometry model

### Body versus surface

Users resize the visible panel body, not the full Wayland surface. The surface
also contains transparent implementation outsets:

```text
surface rectangle
  shadow/safety outset
    visible Apple Music body
  optional provisional hover corridor toward the bar
```

For every candidate body rectangle, derive the surface transaction as:

```text
surface_width  = body_width  + left_outset + right_outset
surface_height = body_height + top_outset  + bottom_outset
surface_origin = body_origin - (left_outset, top_outset)
```

The corner handles are positioned relative to the body rectangle. They must
not drift into the shadow or provisional hover corridor.

### Opposite-corner invariants

Represent the active corner as one of:

```cpp
enum class PanelResizeCorner {
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
};
```

At press time, capture the initial body rectangle and the opposite fixed point:

| Dragged corner | Fixed point |
| --- | --- |
| top-left | initial bottom-right |
| top-right | initial bottom-left |
| bottom-left | initial top-right |
| bottom-right | initial top-left |

Each motion first produces a candidate body rectangle from the pointer and
fixed point. Clamp its size and output bounds, then derive surface dimensions
and margins. Do not incrementally resize from the last request; deriving every
candidate from the immutable transaction origin avoids accumulated rounding
and configure-latency drift.

### Pointer coordinates

Wayland pointer motion is surface-local. During left/top resizing the surface
origin moves, so raw `event.sx/sy` cannot be treated as a stable global
coordinate.

Convert each event to output-local coordinates using the currently configured
surface origin:

```text
surface_origin = panel_body_origin - panel_inset
pointer_output = surface_origin + event_surface_local
```

`m_panelOutputRect`, the insets, and the last acknowledged configure provide
the authoritative transform. Requests may be pending, but geometry used to
interpret an event must always match the surface coordinate space that
produced that event.

The core Wayland implicit pointer grab keeps motion and release routed to the
pressed surface until the button is released. A new private relative-pointer
or compositor resize protocol should not be introduced unless source-level
testing disproves that contract on the supported compositor path.

### Anchors and margins

Keep the current layer-shell anchor selection. Convert the desired surface
origin back into the active anchor's margins after each candidate is clamped:

- a left anchor owns `marginLeft`;
- a right anchor owns `marginRight`;
- a top anchor owns `marginTop`;
- a bottom anchor owns `marginBottom`.

For a trailing-edge anchor, compute the margin from output extent minus the
desired surface trailing edge. Keep unused margins unchanged or zero according
to the current placement branch. Size and margin changes must be committed as
one logical geometry update so niri never observes an intermediate position.

If `LayerSurface` cannot currently batch `set_size` and `set_margin` into one
`wl_surface.commit`, add a narrow `requestGeometry()` method that updates both
before committing. Do not build a generic transaction framework around it.

## Scene and input design

### Resize handles

Add four transparent corner `InputArea`s above the `CefSurfaceNode` in the
Apple Music scene. Each handle should:

- occupy a small logical square inside the body corner, initially 12-16 px;
- have a higher z-index than the CEF input node;
- accept only the primary button;
- use the correct diagonal resize cursor;
- consume its complete press/drag/release sequence;
- be visible to hit testing only while the normal panel is retained.

Keep the hit areas inside the rounded body so no additional input surface is
required. If live use shows that the target is too small, expand it into the
existing transparent surface outset and update `applyPanelInputRegion()`
rather than adding another Wayland surface.

`AppleMusicPanel` should expose only the start-resize intent. `PanelManager`
must own the resize transaction because it owns output geometry, layer-shell
anchors/margins, body outsets, surface lifetime, fullscreen transitions, and
pointer dispatch.

### Resize state

Use one small optional transaction owned by `PanelManager`, for example:

```cpp
struct PanelResizeSession {
  PanelResizeCorner corner;
  PanelOutputRect initialBody;
  PanelOutputRect pendingBody;
  float fixedX;
  float fixedY;
  bool applyPending;
};
```

Do not create a general window-management abstraction. Apple Music is the only
planned resizable panel; extract reusable panel resize policy only if a second
real consumer appears.

While a session is active, `PanelManager::onPointerEvent()` handles motion and
the matching primary-button release before ordinary scene/CEF dispatch. Panel
close, output removal, device recovery, fullscreen entry, or surface teardown
must cancel the session safely.

### Dependent geometry

Every acknowledged size must update the existing owners together:

- `m_panelVisualWidth/Height`;
- `m_panelOutputRect`;
- scene root, reveal clips, content node, background, rounded clip, border,
  shadow, and contact shadow;
- niri compositor blur region;
- normal body input region;
- provisional hover corridor width/height when applicable;
- CEF image and input node dimensions;
- fullscreen-return geometry.

## Frame pacing and CEF behavior

### Coalesce raw motion

Do not call `requestSize()`, rebuild a Vulkan swapchain, or call CEF
`WasResized()` for every raw pointer-motion event.

Motion only replaces `pendingBody` and requests a frame callback. Once per
frame opportunity, apply the newest pending rectangle. This naturally adapts
to the output refresh interval and bounds work when a high-rate mouse produces
multiple motion events per display frame.

There must be at most one unacknowledged layer-shell geometry request. If niri
has not configured the previous request yet, retain only the newest candidate
and submit it immediately after the configure is acknowledged.

### Visual continuity

During a configure-to-CEF-frame gap:

- keep the last complete CEF texture;
- scale and clip it to the current body;
- continue accepting resize motion;
- replace it only when the bridge adopts a complete frame matching the newest
  requested viewport generation.

Do not clear the texture, navigate, recreate the browser, or create a second
CEF browser. Authentication, playback, history, and the direct sampled
DMA-BUF bridge stay intact.

### Apple Music semantic state

The fullscreen path's `preparePresentationResize()` DevTools capture is a
discrete handoff mechanism. It must not run per pointer event or per configure.

Apple Music's injected lyrics resize bridge already reacts to ordinary resize
events. First use that existing ordinary path. If live testing shows lyrics
scroll drift, capture once at resize start and restore once after the final CEF
frame, rather than running the fullscreen capture loop continuously.

## Persistence

Persist only the last completed normal body size, in logical pixels, using the
existing `ConfigService` state store (`state.toml`). Do not add settings-schema
fields or a migration for runtime window geometry.

Suggested owner and value:

```text
owner: apple-music-panel
key: normal-size
value: 1120x720
```

Parsing failure, missing state, or an out-of-range value falls back to the
built-in `1120x720` preference. Write only on successful drag completion, not
for every motion/configure. In-memory normal size updates immediately so
fullscreen exit in the same session restores the new dimensions even if the
state write fails.

## Implementation sequence

### Phase 1: isolate and test body geometry

- Add pure helpers for candidate body rectangles, min/max clamping, output
  clamping, surface outsets, and anchor-to-margin conversion.
- Cover all four corners, every layer-shell anchor combination used by panels,
  fractional logical values, output-edge clamps, and undersized outputs.
- Make no runtime UI changes in this phase.

Exit criterion: geometry tests prove the fixed opposite corner remains stable
within one logical pixel after clamping and round-trip conversion.

### Phase 2: make Apple Music normal size mutable

- Replace the hard-coded preferred size with a mutable normal size initialized
  from state.
- Keep fullscreen dimensions completely separate.
- Clamp the restored size against the selected output during open.
- Preserve the size across close/reopen and fullscreen round trips.

Exit criterion: programmatic size changes correctly drive the existing
layer-shell, Graphite, blur, and CEF resize pipeline without a browser recreate.

### Phase 3: add retained-panel corner handles

- Add the four Apple Music resize hit areas and cursors.
- Enable them only for retained, non-fullscreen presentation.
- Add the `PanelManager` resize session and route press/motion/release/cancel.
- Prevent every resize-owned event from reaching CEF.

Exit criterion: all four corners resize correctly at slow pointer speed and
the opposite corner remains fixed.

### Phase 4: make geometry application presentation-aware

- Coalesce raw motion to Wayland frame opportunities.
- Permit only one outstanding layer-shell geometry configure.
- Batch size and margins into one surface commit.
- Keep only the newest candidate while waiting for configure.
- Update scene, blur, input, hover corridor, and CEF viewport from the
  acknowledged geometry.

Exit criterion: sustained fast resizing remains responsive without an
unbounded configure queue or per-motion swapchain churn.

### Phase 5: persistence and lifecycle

- Persist the final normal size on release.
- Cancel safely on panel close, Escape, output removal, fullscreen entry,
  device recovery, and surface destruction.
- Restore the initial rectangle on Escape.
- Restore the retained size after fullscreen exit without a wrong-size flash.

Exit criterion: normal/fullscreen/normal and close/reopen cycles retain exactly
one normal size and never leak a resize transaction.

### Phase 6: validation and cleanup

- Run unit tests and the release build.
- Run Vulkan synchronization validation while repeatedly resizing.
- Exercise Apple Music playback, animated artwork, scrolling, lyrics, text
  input, popup/select UI, and authentication during and after resize.
- Inspect CEF and bridge logs for resize storms, discarded generations,
  import-cache churn, GPU-process recovery, or blank frames.
- Remove temporary diagnostics after acceptance.

Exit criterion: the acceptance list below passes and the implementation adds
no fallback renderer, second browser, or compositor-specific resize protocol.

## Expected files

Primary changes:

- `src/shell/apple_music/apple_music_panel.h`
- `src/shell/apple_music/apple_music_panel.cpp`
- `src/shell/panel/panel_manager.h`
- `src/shell/panel/panel_manager.cpp`
- `src/wayland/layer_surface.h`
- `src/wayland/layer_surface.cpp`

Possible narrow supporting changes:

- a small pure panel-resize geometry helper and unit test under `src/shell/panel`
  and `tests`;
- `src/config/config_service.*` only if the existing public state-string API is
  insufficient (it currently appears sufficient).

No planned changes:

- Chromium or CEF patches;
- `CefGpuFrameBridge` synchronization or allocation;
- Nucleus render SDK or Skia;
- niri or Smithay;
- settings schema or configuration migration.

## Acceptance criteria

- All four corners show the correct resize cursor and resize in the expected
  direction.
- The opposite corner stays visually fixed during a drag.
- Hover-only Apple Music remains provisional and has no resize affordance.
- Clicking the topbar item retains the existing preview and enables resizing.
- Fullscreen has no handles and returns to the exact retained normal size.
- The panel never crosses output bounds and remains usable after changing
  monitor scale or output geometry.
- The bar-to-panel hover corridor remains correct after resizing.
- The niri blur region, rounded corners, border, and shadow track every accepted
  size without stale strips or opaque flashes.
- CEF input coordinates remain correct at every size and fractional scale.
- Apple Music does not reload or lose authentication, playback, page state, or
  lyrics position.
- Animated artwork remains hardware decoded, corruption-free, and flicker-free.
- Fast resizing does not continuously grow pending configures, Vulkan
  resources, DMA-BUF imports, file descriptors, or retained swapchain
  generations.
- Vulkan validation reports no lifetime, synchronization, layout, or ownership
  errors.

## Risks and mitigations

### Configure and swapchain churn

Risk: raw mouse motion can request more sizes than niri and Graphite can
configure/present.

Mitigation: one latest pending rectangle, one outstanding configure, and one
application per frame opportunity.

### Left/top coordinate feedback

Risk: moving the surface origin changes surface-local pointer coordinates and
can make left/top dragging oscillate.

Mitigation: convert each event through the currently acknowledged surface
origin and derive candidates from the immutable transaction origin/fixed
corner. Never treat local coordinates as output coordinates.

### CEF layout cost

Risk: Apple Music may perform expensive layout for every viewport change.

Mitigation: coalesce at the panel boundary, keep the previous complete frame
visible, and never call the fullscreen semantic capture per motion event.

### Input leakage

Risk: a resize press reaches the page and activates an Apple Music control
under a corner.

Mitigation: resize handles live above CEF and own the complete implicit-grab
sequence until release/cancel.

### Over-generalization

Risk: implementing a generic window manager inside `PanelManager` before any
other panel needs resize behavior.

Mitigation: land the smallest Apple Music-specific policy with pure geometry
helpers. Generalize only when a second concrete panel becomes resizable.

