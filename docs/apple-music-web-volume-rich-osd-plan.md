# Apple Music Web Volume and Rich System Volume OSD Plan

Last updated: 2026-07-20

Status: design complete; implementation has not started.

## Decision

Implement two related features with deliberately separate feedback surfaces:

1. Scrolling the Apple Music topbar item changes Apple Music's own in-page
   volume control. The hover-opened Apple Music panel remains visible and the
   SPA reveals its existing volume UI. No Noctalia volume OSD appears for this
   action.
2. System output-volume changes continue to use Noctalia's existing OSD
   surface. Its `OsdKind::Volume` presentation becomes a mouse-interactive,
   media-aware card when useful MPRIS metadata exists, and remains a compact
   interactive volume control when it does not.

The two volume domains must never be conflated:

| Trigger/control | Authority | Feedback |
| --- | --- | --- |
| wheel over Apple Music bar item | Apple Music webpage slider | Apple Music's own revealed volume UI |
| rich OSD volume slider | PipeWire default output sink | Noctalia rich volume OSD |
| wheel over ordinary output-volume bar item | PipeWire default output sink | Noctalia rich/compact volume OSD |
| microphone volume | PipeWire default input source | existing passive microphone OSD |

Do not fall back from Apple webpage volume to MPRIS volume, process-stream
volume, or the default PipeWire sink. If the page control cannot be found, the
wheel event is consumed safely and no unrelated volume is changed.

No new xdg-popup, Chromium/CEF patch, renderer backend, or compositor protocol
is required. The existing `noctalia-osd` layer surface already renders above
the Apple Music panel; it needs a richer volume scene and a bounded input
region.

## Investigation findings

### Apple Music bar behavior

`AppleMusicWidget` is a specialized `MediaWidget` pinned to
`currentProcessChromiumMprisBusName()`. Its pointer-enter handler already opens
the Apple Music panel as a provisional hover preview, while a click retains the
panel.

Therefore a wheel gesture over that item normally occurs while the real Apple
Music page is already visible. Creating a second Noctalia popup would duplicate
feedback and compete with the hover panel. The page-owned slider is the right
feedback surface.

`InputArea::scrollSteps()` already provides detent-normalized wheel input and
handles high-resolution wheels and touchpads. The Apple item can reuse it with
the same direction convention as the existing `VolumeWidget`: positive
Wayland vertical steps mean scroll down, so they reduce volume.

### CEF page integration

`CefService::execJs()` and the existing Apple Music scripts in
`noctalia_cef_app.cpp` prove that main-frame page integration is already
available. The current scripts cover transparent styling and lyrics resize,
but there is no Apple volume adapter.

The authenticated SPA's interactive volume control is not present in the
server-rendered public page. Its runtime custom-element/shadow-DOM structure
must be inspected once through the existing
`NOCTALIA_CEF_REMOTE_DEBUGGING_PORT` seam before finalizing the adapter. The
permanent bridge must select the control by semantics and stable custom-element
structure, not generated CSS class names.

The adapter must operate the actual page control. Directly mutating an
`HTMLMediaElement.volume` value would bypass the visible slider and violate the
feature's contract.

### Existing system OSD

`OsdOverlay` already owns one `noctalia-osd` overlay-layer surface per selected
output, handles configured position/orientation, animates show/hide, and
publishes a rounded niri blur region. `AudioOsd` already routes output-volume
changes into `OsdKind::Volume`.

The existing OSD is intentionally passive:

- every surface receives an empty Wayland input region;
- the scene contains a glyph, `ProgressBar`, and value label;
- there is no `InputDispatcher` or pointer routing.

The correct change is a rich presentation inside this owner, not a parallel
volume-overlay manager. Microphone and all non-volume OSD kinds remain on their
current passive path.

### Reusable media/UI machinery

The repository already provides almost all rich-card building blocks:

- `MprisPlayerInfo`: title, artists, album, artwork URL, position, duration,
  playback state, and transport/seek capabilities;
- `MprisService`: exact-player play/pause, previous, next, and seek operations;
- `mpris_art`: asynchronous artwork resolution and caching;
- `Slider`: mouse drag, wheel adjustment, and value/drag-end callbacks;
- media-tab pending-seek handling that prevents the progress thumb snapping
  backward while MPRIS settles.

`MprisPlayerInfo` does not currently parse `xesam:contentCreated`, and a live
snapshot of Chromium's Apple Music MPRIS metadata did not include a year. Year
therefore needs a two-level source: standard MPRIS metadata when available,
then narrowly scoped Apple Music supplemental metadata from the page bridge.
Never infer or fabricate a year.

## Product behavior

### Apple Music topbar wheel

- Hovering the Apple Music item continues to open the current provisional
  panel.
- Each vertical wheel detent changes the page slider by 5 percentage points.
- Scroll up increases and scroll down decreases, clamped to `[0, 1]`.
- The SPA reveals the same volume control the user can manipulate inside Apple
  Music and keeps it visible according to its normal hover/focus behavior.
- Repeated wheel input refreshes the SPA's own reveal lifetime.
- No Noctalia system-volume OSD is shown.
- The action does not retain a provisional panel. Leaving the item/panel still
  follows the existing hover-preview dismissal policy.
- A click-retained panel behaves identically except that its normal retained
  lifetime remains in force.
- The wheel event never reaches the Apple Music document as a coordinate-based
  CEF wheel event; it invokes the semantic page adapter instead.
- When the bridge is unavailable, consume the event, leave the panel usable,
  and log one rate-limited warning. Do not adjust any other volume domain.

Do not synthesize a fake CEF pointer position over the page slider. The real
pointer remains over the bar, and coordinate spoofing would fight page hover,
CEF cursor state, and later pointer entry into the panel. Dispatch DOM-level
events to the page-owned control instead.

### Rich system-volume OSD

When output volume changes and a useful media session exists, show a landscape
card containing:

- large album artwork;
- title;
- artist;
- album and year;
- previous, play/pause, and next controls;
- elapsed/total time labels and a progress track;
- an interactive output-volume slider, volume glyph, and percentage.

The progress track is seekable only when the displayed MPRIS player advertises
`CanSeek`; otherwise it is read-only. Transport buttons are enabled from that
player's exact capabilities.

When no MPRIS player has meaningful loaded media, show only the compact
interactive system-volume row. A paused player with track metadata still uses
the rich card.

The rich card is a presentation variant of `OsdKind::Volume`. Existing
brightness, microphone, media-change, privacy, and other OSD presentations do
not gain input or change dimensions.

### Lifetime and interaction

- A volume change opens the OSD and resets its current hide delay.
- Entering the rich/compact volume card pauses auto-dismiss.
- Slider dragging and button presses keep it open.
- Leaving the card restarts a short dismissal delay.
- The OSD does not take keyboard focus or create an exclusive zone.
- Pointer input is accepted only inside the currently revealed card; the rest
  of the transparent layer surface remains click-through.
- A hide animation cannot start while a slider owns an implicit pointer grab.
- Output removal, lockscreen entry, config-driven surface recreation, or
  shutdown cancels interaction before destroying the scene.

The configured OSD monitor and position rules remain authoritative. The rich
card uses a fixed horizontal composition even if the compact OSD is configured
vertically; forcing a large media card into a vertical progress-pill layout
would produce a poor and needlessly complex UI.

## Media selection

Use a pure deterministic selector for the rich system-volume presentation:

1. If this process's embedded Apple Music MPRIS player is `Playing` and has
   meaningful metadata, select it.
2. Otherwise select `MprisService::activePlayer()` when it has meaningful
   metadata. This intentionally includes paused players.
3. Otherwise use the compact system-volume presentation.

Meaningful media is not merely the existence of an MPRIS bus name. At least
one track-bearing field must exist, such as title, artist, album, artwork, or a
positive duration.

Apple Music priority is limited to the playing case. A paused Apple session
must not displace another actively playing source chosen by the normal MPRIS
policy.

Once the card is built, every transport and seek command targets the displayed
player's captured bus name, never whichever player happens to be active at
click time. Re-evaluate selection when MPRIS state changes; keep stable ties to
avoid card flicker.

## Apple Music page bridge

### Runtime audit

Before implementation, inspect an authenticated Apple Music context and record:

- the stable host/custom-element path to the page volume control;
- whether the interactive element is a native range input, ARIA slider, or a
  custom element with a public value property;
- the element that owns reveal/hover state;
- the exact input/change or component event sequence used by the SPA;
- the stable current-track object or semantic DOM source that exposes release
  year when Chromium MPRIS omits it.

This is a one-time implementation input, not a permanent probe, compatibility
matrix, or generated capability contract.

### Permanent adapter

Install one main-frame-only bridge for `https://music.apple.com/`, alongside
the existing transparent-theme and lyrics bridges. It should expose a narrow
page method such as:

```js
globalThis.__noctaliaAppleMusicShellV1.adjustVolumeAndReveal(delta)
```

The method must:

1. locate or rebind the semantic page volume control;
2. read and normalize its actual min/max/current value;
3. clamp and apply the requested delta through the control's real setter;
4. dispatch the event sequence consumed by the SPA;
5. trigger the page's real reveal state without permanently forcing CSS;
6. read back the accepted value on the next animation frame.

Use a document mutation observer only to rebind when SPA navigation replaces
the control. Do not poll continuously. Walk open shadow roots only where the
runtime audit proves they are used.

If the volume UI normally reveals through pointer/focus events, dispatch that
sequence to its owning component and release synthetic hover/focus after the
site's normal timeout. Do not add a cloned slider or an always-visible CSS
override.

### Native boundary

Add a narrow `CefService::adjustAppleMusicPageVolume(double delta)` API. It
validates a finite delta, restricts execution to the Apple Music main frame,
invokes the installed bridge, and requests an external BeginFrame so the
revealed control paints promptly.

No generic JavaScript command bus is needed.

For the optional Apple release year, use one typed renderer-to-browser process
message containing a track signature and year. Accept it only from the main
Apple Music frame, clamp the year to a sane range, and apply it only when the
signature still matches the MPRIS title/artist/album. This prevents a delayed
SPA message from labeling the next track incorrectly.

Prefer `xesam:contentCreated` whenever MPRIS supplies it. The page bridge is an
Apple-specific supplement, not a replacement for standard media metadata.

## OSD architecture

### Presentation variants

Keep `OsdContent` for the existing passive presentations and add an internal
volume presentation model, for example:

```cpp
enum class VolumeOsdPresentation {
  Compact,
  RichMedia,
};

struct VolumeOsdMedia {
  std::string busName;
  std::string title;
  std::string artist;
  std::string album;
  std::optional<int> year;
  std::string artUrl;
  std::string playbackStatus;
  std::int64_t positionUs = 0;
  std::int64_t lengthUs = 0;
  bool canGoPrevious = false;
  bool canPlay = false;
  bool canPause = false;
  bool canGoNext = false;
  bool canSeek = false;
};
```

This is an in-process view model, not a protocol or schema. Build it from
`MprisService` only while `m_content.kind == OsdKind::Volume`.

Switching between compact and rich geometry must recreate or atomically resize
the OSD surface before publishing the matching scene buffer. Never let the
compositor scale a stale compact buffer into rich dimensions.

### Services and updates

Give `OsdOverlay` only the services required by the rich volume variant:

- `PipeWireService` for the current default sink and volume writes;
- `MprisService` for media selection and exact-player controls;
- `HttpClient` for the existing artwork resolver;
- `ConfigService` for overdrive and visual settings.

The application's existing MPRIS change callback should notify the OSD when it
is visible. Do not replace `MprisService`'s current callback ownership with a
new signal framework solely for this feature.

While rich media is visible, update projected position at a modest cadence
(about four times per second). Projection is local and does not require a D-Bus
round trip. Stop the timer immediately when the card hides.

Reuse the media tab's pending-seek/settle behavior so dragging the timeline
does not snap back before MPRIS reports the new position. Do not overwrite the
volume or progress thumb while its slider is being dragged.

### System-volume slider

The volume slider controls the current default PipeWire output sink:

- range `[0, 1]` normally;
- range `[0, 1.5]` when audio overdrive is enabled;
- 1% drag precision and the existing configured/detent wheel step;
- optimistic visual update reconciled by the normal PipeWire change callback;
- over-limit values retain the existing error accent behavior.

Changing this slider naturally updates `AudioOsd`. Ensure that the echo extends
the current OSD lifetime without rebuilding the scene or restarting its reveal
animation on every drag event.

### Scene and input

Extend each `OsdOverlay::Instance` with an `InputDispatcher` and pointers to
the rich controls. Route pointer events for an OSD-owned `wl_surface` through
`Application::initInputDispatch()` before bar, dock, and panel dispatch.

For `OsdKind::Volume`, publish a Wayland input region matching the animated
card rectangle. Recompute it with reveal geometry so hidden transparent pixels
never steal input. For all other OSD kinds, retain the current empty region.

The scene should reuse normal Noctalia controls and Graphite resources:

- `Image` plus `mpris_art` for artwork;
- `Label` for metadata and time text;
- `Button` for transport;
- `Slider` for timeline and volume;
- the existing rounded surface fill, border, animation, and compositor blur.

Keep one OSD surface/model per configured output, as today. A pointer interaction
may keep that instance open while instances on other outputs finish their
normal timeout.

## Apple bar integration

Give `AppleMusicWidget` access to the narrow CEF page-volume operation through
`BarServices`/`WidgetFactory`; CEF types remain hidden behind the existing
`CefService` pImpl header.

`MediaWidget` should expose a small protected input-configuration hook or its
`InputArea` to derived classes so `AppleMusicWidget` can install an axis handler
without duplicating the media widget scene. Do not add an Apple-mode boolean to
generic media input behavior.

The handler:

1. accepts only vertical axis input;
2. reads `scrollSteps()`;
3. returns unconsumed when no full detent is available;
4. invokes `adjustAppleMusicPageVolume(-steps * 0.05)`;
5. consumes a real detent even when the bridge is temporarily unavailable.

The ordinary `VolumeWidget` keeps its existing PipeWire scroll path. Its
resulting `AudioOsd::showOutput()` call automatically activates the upgraded
rich/compact system-volume OSD, so it needs no separate overlay callback.

## Implementation sequence

### Phase 1: audit and implement the Apple page adapter

- Inspect the authenticated runtime volume component and release-year source.
- Add the semantic volume adjustment/reveal bridge.
- Add the narrow `CefService` operation.
- Add bounded supplemental-year messaging only if MPRIS does not supply it.
- Verify that page UI, actual audio gain, and bridge readback agree.

Exit criterion: wheel-driven test calls move the real Apple slider, reveal its
native UI, and leave PipeWire sink volume unchanged.

### Phase 2: wire Apple topbar wheel input

- Add the derived-media input hook.
- Consume detent-normalized vertical wheel input on `AppleMusicWidget`.
- Preserve provisional versus retained panel lifetime exactly as today.
- Ensure no system OSD is triggered.

Exit criterion: hovering and scrolling the topbar item visibly changes only
Apple's own volume, with the panel remaining provisional unless clicked.

### Phase 3: isolate rich-volume model and selection

- Add pure player-selection and meaningful-metadata helpers.
- Parse standard MPRIS creation date/year metadata.
- Merge a matching Apple supplemental year when available.
- Add unit tests for playing Apple priority, paused media, other playing media,
  stale supplemental metadata, and no-media compact fallback.

Exit criterion: the presentation model and exact control bus are deterministic
for every player-state combination.

### Phase 4: build the rich and compact interactive volume scenes

- Add rich card dimensions and scene nodes to `OsdOverlay`.
- Replace the compact `ProgressBar` with an interactive `Slider` only for output
  volume.
- Add artwork loading, metadata, transport, progress, and time labels.
- Keep the current compact layout when no useful media exists.
- Reuse the existing OSD fill, border, blur, positioning, and reveal style.

Exit criterion: both presentation variants render correctly at every UI and
fractional output scale without stale-buffer stretching.

### Phase 5: input, timing, and service integration

- Route OSD pointer events through an instance `InputDispatcher`.
- Publish only the animated card as the input region.
- Pause dismissal while hovered or dragging and resume it on leave.
- Wire volume, transport, and seek callbacks to their exact authorities.
- Refresh MPRIS metadata and projected position only while rich OSD is visible.
- Cancel all interaction cleanly on surface destruction/output changes.

Exit criterion: controls remain usable over an open Apple Music panel, no
transparent OSD pixels steal clicks, and no input escapes to CEF underneath.

### Phase 6: validation and cleanup

- Build and run the optimized configuration.
- Exercise Apple page volume while hover-opened, retained, paused, and playing.
- Exercise hardware keys, ordinary volume-widget wheel, and OSD slider drag.
- Test Apple playing priority, another active player, paused media, and no MPRIS
  media.
- Test all configured OSD positions, multi-monitor selection, output removal,
  fractional scale, panel overlap, and lockscreen transitions.
- Run Vulkan validation for the new Graphite scene and artwork lifecycle.
- Remove temporary DOM diagnostics and remote-debug setup after acceptance.

Exit criterion: the acceptance list below passes with no Chromium patch or
parallel overlay implementation.

## Expected files

Primary changes:

- `src/cef/noctalia_cef_app.h`
- `src/cef/noctalia_cef_app.cpp`
- `src/cef/cef_service.h`
- `src/cef/cef_service.cpp`
- `src/shell/bar/bar_services.h`
- `src/shell/bar/widget_factory.*`
- `src/shell/bar/widgets/media_widget.*`
- `src/shell/bar/widgets/apple_music_widget.*`
- `src/shell/osd/osd_overlay.h`
- `src/shell/osd/osd_overlay.cpp`
- `src/dbus/mpris/mpris_service.*`
- `src/app/application.h`
- `src/app/application_ui.cpp`
- `src/app/application_services.cpp`

Possible narrow supporting changes:

- a pure rich-volume player-selection helper and unit test;
- a focused Apple page bridge source if the runtime adapter is too large to
  remain readable beside the existing injected scripts;
- localization keys for compact/rich accessibility labels and missing metadata.

No planned changes:

- Chromium or CEF source patches;
- `CefGpuFrameBridge`, Graphite synchronization, or DMA-BUF ownership;
- Nucleus render SDK or Skia;
- niri or Smithay;
- system mixer policy or per-process PipeWire routing;
- a second overlay/popup surface manager.

## Acceptance criteria

- Apple topbar wheel changes the actual Apple Music webpage slider by the
  expected amount and reveals the SPA's native volume UI.
- Apple page volume changes neither the default PipeWire sink nor an MPRIS
  volume property as a substitute.
- No Noctalia OSD appears for Apple page-volume changes.
- Apple hover previews remain provisional; scrolling does not pin them.
- System output-volume changes show one rich OSD when meaningful MPRIS media is
  loaded, including when that media is paused.
- A playing embedded Apple Music session wins the rich-card selection.
- Without loaded MPRIS media, the same OSD shows only the compact interactive
  system-volume control.
- Album art, title, artist, album, year when available, playback state, elapsed
  time, and duration update without rebuilding the surface unnecessarily.
- Previous/play-pause/next and seek always control the player displayed in the
  card.
- The volume slider always controls the default output sink and respects the
  configured overdrive limit.
- The rich OSD remains mouse-interactive above an open Apple Music panel and no
  pointer event reaches CEF underneath it.
- Hovering or dragging pauses dismissal; leaving resumes it; hidden transparent
  pixels never block other clients.
- Artwork decode/upload and position timers stop when the OSD is destroyed.
- Multi-monitor, fractional scaling, output hotplug, and every OSD position
  produce correct geometry and blur regions.
- Vulkan validation reports no resource, lifetime, or synchronization errors.

## Risks and mitigations

### Apple SPA structure changes

Risk: generated classes or component internals change and the volume adapter
silently stops working.

Mitigation: select by accessible semantics and stable custom-element structure,
rebind after SPA mutations, validate readback, and fail without touching another
volume domain. Keep the bridge small enough to update from a fresh runtime
inspection.

### Synthetic reveal fighting real pointer state

Risk: spoofed CEF coordinates or permanent CSS hover state causes cursor
fighting and sticky controls.

Mitigation: dispatch only the audited DOM-level component events, never move
CEF's pointer, and let the page's own reveal timeout clear the UI.

### OSD input stealing clicks

Risk: the transparent extent of an overlay-layer surface blocks the Apple panel
or applications beneath it.

Mitigation: publish a card-only input region synchronized with reveal geometry;
all non-volume OSDs retain an empty region.

### Player switches during interaction

Risk: an active-player change causes a button release or seek to affect a
different media session.

Mitigation: callbacks capture the displayed bus name and scene generations;
selection changes rebuild only after outstanding pointer interaction ends.

### Async artwork lifetime

Risk: a download/decode callback targets an OSD scene destroyed by its timeout.

Mitigation: use the existing weak lifetime-guard pattern and perform Graphite
upload only on the main render context.

### Drag update storms

Risk: PipeWire echoes and MPRIS position updates repeatedly rebuild the rich
scene or restart reveal animations.

Mitigation: mutate stable scene nodes in place, ignore service echoes within the
normal epsilon, retain optimistic slider state while dragging, and extend only
the hide deadline.
