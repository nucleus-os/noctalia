# Discord Web Panel and Reusable CEF Panel Foundation

Last updated: 2026-07-20

Status: reusable foundation implemented and authenticated rendering validated;
microphone/camera hardware acceptance remains a manual follow-up

## Implementation status

Implemented on 2026-07-20:

- `CefService` is process-wide only and owns a registry of independent
  `CefBrowserSession` objects;
- Apple Music and Discord have separate clients, schedulers, GPU bridges,
  textures, input state, visibility policy, recovery state, and diagnostics;
- `CefSurfaceNode` is session-bound and `WebPanel` owns shared full-bleed
  rendering, lifecycle, retained-frame, loading, retry, placement, and glass
  behavior;
- Discord is a lazy persistent session launched by a left/start
  `brand-discord` topbar item and the current user configuration enables it;
- Apple and Discord renderer integrations live in separate translation units
  behind an exact HTTPS-origin dispatcher with focused policy tests;
- Discord media capture is fail-closed except for independent, remembered,
  origin-scoped microphone/camera decisions presented inside the panel;
- untrusted top-level navigation is blocked, user-initiated external links
  open through the system browser, and trusted auxiliary windowless browsers
  are presented in a bounded native sheet stack;
- file uploads use Noctalia's native file dialog, downloads use its native Save
  dialog, and Chromium's common context-menu commands are presented through a
  native Wayland menu;
- CEF's default link/text/image context-menu commands are presented with
  Noctalia's native Wayland menu and returned to Chromium for execution;
- Apple playback/metadata ownership comes from its own Media Session bridge,
  so Discord media cannot drive Apple hidden refresh or topbar content;
- hidden Discord has no visual heartbeat, while the existing Apple-only parked
  playback policy remains session-local;
- shutdown returns every session's pending GPU ownership before closing its
  browser, and the Chromium transport has an explicit terminal abandonment
  path for exported reads whose backing is being destroyed.

Runtime acceptance on 2026-07-20 used the release build with Vulkan
synchronization validation enabled. The authenticated Discord session produced
accelerated frames, switching to a separately retained Apple Music session and
back preserved both sessions, and clean shutdown returned every accepted frame
without a GPU-process restart, ownership error, cache eviction, or leaked
active import. The two bridge summaries reported three stable imported buffers
per session and zero cache evictions.

The authenticated Discord DOM has been audited through the opt-in debugging
endpoint. Its current nested dark/light theme containers redefine semantic
surface variables below `:root`, so the integration applies the palette to
those containers as well as the transparent application roots; it does not
depend on generated class names.

Still requiring live acceptance rather than more speculative code: tune the
resulting palette visually, validate microphone voice/camera hardware paths,
verify upload/download and native context-menu behavior against Discord's real
UI, and identify any Discord-owned authentication flow that truly needs an
auxiliary windowless popup. Ordinary external links already have explicit
behavior. Multi-file selection, native notification presentation, and
portal-backed screen sharing remain separate follow-up slices rather than
unsafe blanket grants.

## Decision

Add Discord as a second persistent, accelerated, windowless CEF panel. It will
open from a `brand-discord` item in the start/left section of the top bar and
use the same visual contract as the Apple Music panel:

- full-bleed web content inside Noctalia's rounded panel shape;
- a transparent Chromium root;
- semitransparent site tints rather than opaque page backgrounds;
- the panel surface's niri blur region visible through those pixels;
- native Noctalia placement, animation, focus, input, and lifecycle ownership;
- Graphite/Dawn/Vulkan production and Noctalia's direct Vulkan/Graphite
  DMA-BUF consumer, with no CPU, GLES, or native Chromium-window fallback.

Discord is also the first real second top-level browser session. It must not be
implemented by cloning the single-browser `CefService` or by adding another
CEF runtime. The implementation will first finish the per-browser session
extraction already designed in `cef-windowless-multi-browser-plan.md`, then put
Apple Music and Discord on a small reusable web-panel host.

The reusable layer will be deliberately code-defined. Adding a supported site
should require a profile, a site integration, and—only when necessary—a small
specialized panel or widget. It will not expose an arbitrary URL/script system,
runtime capability negotiation, a versioned internal protocol, or another
configuration schema for code that is compiled together.

## Product behavior

### Topbar entry

The built-in widget type is `discord`. Its default presentation is an icon-only
`brand-discord` button with an accessible label and tooltip.

- It is placed in the `start` list of the user's top bar, not forced into every
  user's source defaults.
- A left click toggles the Discord panel.
- The panel opens near the clicked item and is clamped inside the output.
- Hover-open is off initially. Apple Music's provisional hover behavior is a
  site/product choice, not a default behavior of web panels.
- The widget can later gain an unread badge through a Discord site event; that
  is not required to establish the panel.

For the current user configuration, implementation ends by making the existing
implicit start section explicit and adding Discord without disturbing its
other items:

```toml
[bar.default]
start = ["discord", "launcher", "wallpaper", "workspaces"]
```

### Panel behavior

The initial Discord panel is a floating, full-bleed panel around 1180×760
logical pixels, subject to output-size clamping and live visual adjustment.
It uses:

- overlay layer;
- exclusive keyboard focus while open;
- normal Noctalia open/close animation and click-away dismissal;
- rounded Graphite clipping matching the outer panel;
- mouse, wheel, keyboard, IME, clipboard, history buttons, and cursor routing
  through the shared CEF surface node;
- a retained session, history, cookies, authentication, and last complete
  texture across ordinary panel closes.

Discord does not initially inherit Apple Music's fullscreen toplevel handoff,
lyrics resize bridge, playback-aware hidden heartbeat, or hover-preview
lifetime. Those remain Apple-specific behavior.

The first open may be a cold browser load. Instead of exposing an empty panel,
the shared host shows a native loading/error layer until the first usable CEF
frame arrives. After that first load, closing the panel parks rendering but
retains the browser and last complete frame, making subsequent opens immediate.
Do not preload the full Discord application during shell startup until runtime
memory and startup impact justify it.

### Glass appearance

The visible composition is:

```text
Discord semantic tint/content (CEF premultiplied RGBA)
    over Noctalia panel tint and rounded shape
    over niri-provided blur of the real desktop below the panel
```

Noctalia does not screenshot, copy, or inject the desktop into Chromium. The
existing panel blur region supplies compositor blur. Discord's site integration
only makes the appropriate root and structural surfaces translucent while
preserving enough tint for contrast.

The CSS must not apply opacity or a filter to the whole application root. That
would flatten child compositing, alter text opacity, and create unnecessary
offscreen layers. Prefer Discord's semantic color variables and a small number
of stable structural roots, changing opaque colors to explicit alpha colors.

## Historical baseline and constraints

### CEF was single-browser before this implementation

`CefService::Impl` previously owned one browser, client, GPU bridge, texture,
frame scheduler, dimensions, pointer state, cursor, renderer-recovery state,
and callback set. `CefSurfaceNode` forwards every operation to that process-wide
object. A second browser would therefore alias producer frames, release fences,
BeginFrame acknowledgements, input, size, and recovery state.

`OnBeforePopup()` is correctly fail-closed today. It blocks unhosted browsers
because there is no safe second session yet. This containment must remain until
the per-session implementation exists.

### Apple Music mixes shared and site-specific behavior

The reusable parts of `AppleMusicPanel` are:

- CEF surface creation and callback attachment;
- frame-ready update/redraw requests;
- compositor-paced frame opportunities and presentation feedback;
- full-bleed sizing, texture adoption, rounded clipping, and initial focus;
- retained-browser attach/detach behavior;
- transparent panel background policy.

The Apple-only parts are:

- `https://music.apple.com/` navigation;
- hover-preview behavior in `AppleMusicWidget`;
- the process MPRIS-backed title/art widget;
- playback-aware 1 Hz parked refresh;
- fullscreen presentation and niri toplevel transfer;
- timed-lyrics resize capture;
- Apple DOM/CSS integration.

The refactor must extract only the first group. Turning all Apple behavior into
generic flags would make the common layer harder to understand than two small
site subclasses.

### Site scripts are centralized in the CEF application

`NoctaliaCefApp::OnContextCreated()` currently matches Apple Music's origin and
injects two large script strings from `noctalia_cef_app.cpp`. Adding Discord
there would make one application file a growing collection of unrelated DOM
knowledge.

Site integrations need separate translation units and one small dispatcher.
They remain compiled resources so the CEF renderer helper does not depend on a
runtime asset search path or silently lose its styling when an install is
incomplete.

### Process-wide MPRIS is not site identity

Chromium publishes one MPRIS service for this browser process. Current code
renames that identity to Apple Music and uses it both for `AppleMusicWidget`
and Apple Music's hidden playback refresh. With two top-level web applications,
Discord audio or an embedded video can become Chromium's active media session.
The process bus can no longer prove that the active media belongs to Apple.

Before Discord media is accepted, Apple-specific state must come from its own
browser session or page integration. Process-wide MPRIS remains useful for the
general media picker/OSD, but it cannot remain the ownership key for an
Apple-only widget or scheduling decision.

### The transparency transport already exists

The production CEF browser already requests a transparent background and
publishes premultiplied RGBA through accelerated OSR. `PanelManager` already
sets a rounded niri blur region for panel surfaces. `AppleMusicPanel` proves
that a full-bleed transparent CEF image can reveal that blur correctly.

Discord therefore needs no new Vulkan, Graphite, niri, or CEF transport. The
rendering work is session isolation plus site CSS, not another renderer.

## Target architecture

```text
Application
  |
  +-- CefService                         one process-wide CEF runtime
  |     - initialize/shutdown and message pump
  |     - shared persistent request context/profile
  |     - GraphicsDevice binding
  |     - live/pending browser registry
  |
  +-- CefBrowserSession (Apple Music)    one producer and one consumer domain
  |     - CefBrowser + client
  |     - scheduler/watchdog/recovery
  |     - CefGpuFrameBridge + texture
  |     - size/scale/visibility/input/cursor
  |
  +-- CefBrowserSession (Discord)        completely independent state
  |     - same per-session machinery
  |
  +-- auxiliary popup sessions           only when explicitly presented

WebPanel
  +-- shared surface/lifecycle/rendering/loading behavior
      |
      +-- AppleMusicPanel
      |     - fullscreen/lyrics/playback specializations
      |
      +-- DiscordPanel
            - initially no special native behavior

WebPanelLauncherWidget
  +-- Discord built-in widget

MediaWidget
  +-- AppleMusicWidget remains specialized
```

### `CefService`: process-wide runtime only

Keep in `CefService`:

- CEF initialization, shutdown, distribution paths, and external message pump;
- the shared persistent profile/request context;
- process-wide command-line configuration and GPU selection;
- GraphicsDevice attachment;
- a registry of top-level and popup sessions;
- ordered device-rebuild and shutdown iteration.

Replace the implicit primary browser API with a direct creation method such as:

```cpp
std::shared_ptr<CefBrowserSession>
CefService::createSession(WebPanelSite site, CefBrowserSessionOptions options);
```

There is no privileged singleton “primary” session after the migration. Apple
Music and Discord are both ordinary top-level sessions. Popup sessions retain
their opener relationship but use the same per-session frame contract.

### `CefBrowserSession`: all per-browser state

Move into a CEF-free public session object with a private implementation:

- browser/client and pending initial URL;
- logical size and device scale;
- attached, focused, loading, failed, and first-frame state;
- external BeginFrame scheduler, acknowledgement watchdog, and presentation
  interval;
- renderer recovery;
- one `CefGpuFrameBridge`, stable texture handle, and dirty state;
- pointer coalescing, pressed buttons, cursor, and callbacks;
- navigation, JavaScript execution, history, and close;
- optional parent session and popup metadata;
- site identity for policy and diagnostics.

`CefSurfaceNode` will accept a `shared_ptr<CefBrowserSession>` rather than a
`CefService&`. Every input, frame, and release operation then has an
unambiguous destination.

### `WebPanelProfile`: small code-defined policy

Use a compact enum and constexpr profile table, not a runtime plugin schema:

```cpp
enum class WebPanelSite { AppleMusic, Discord };

struct WebPanelProfile {
  WebPanelSite site;
  std::string_view panelId;
  std::string_view startUrl;
  std::span<const std::string_view> trustedTopLevelOrigins;
  float preferredWidth;
  float preferredHeight;
  HiddenFramePolicy hiddenFrames;
};
```

The profile describes stable common facts. Glyphs and topbar presentation stay
in widget code; fullscreen and lyrics stay in Apple code; Discord permissions
stay in Discord policy. Do not grow the profile into a bag of callbacks and
boolean capabilities.

### `WebPanel`: shared native host

The base panel owns a session and implements:

- create/attach/detach;
- first navigation;
- frame-ready update/redraw callbacks;
- frame opportunity and presentation forwarding;
- full-bleed layout and texture adoption;
- rounded clipping and initial focus;
- native loading/failure overlay;
- common floating placement and glass-background behavior.

It exposes narrow protected hooks for the exceptional behavior that already
exists, such as Apple Music's semantic viewport capture. `DiscordPanel` should
initially be only a constructor selecting the Discord profile. If it remains
empty after acceptance, it may be replaced with a direct `WebPanel` instance
at registration; do not require a subclass merely for symmetry.

`PanelManager::AppleMusicFullscreenHost` remains Apple-specific. Generalizing
that large state machine before a second site needs fullscreen would be harmful.

### `WebPanelLauncherWidget`: common icon launcher

Extract the tiny icon/custom-image panel-toggle pattern already repeated by
launcher and control-center widgets. It accepts a panel ID, glyph, tooltip, and
optional context. Discord uses it with `brand-discord`.

Apple Music does not use it because its topbar content and interactions are
driven by media state. A future simple website panel can use the launcher
without adding another near-identical widget class.

### Site integration dispatcher

Split renderer-side code into:

```text
src/cef/site_integrations/
  site_integration.h/.cpp        origin matching and dispatch only
  apple_music_integration.cpp   transparent theme and lyrics bridge
  discord_integration.cpp       Discord glass theme and optional page events
```

The main-frame context callback asks the dispatcher for scripts matching the
current trusted origin. Each site owns its selectors, CSS variables, observers,
and bridge names. Scripts are idempotent and carry a site-specific revision in
their DOM IDs.

Do not put Noctalia panel geometry, CEF session state, or another site's DOM
knowledge into these integrations. Do not inject into untrusted origins reached
through navigation or authentication.

## Discord glass integration

### Runtime audit first

Discord's generated class names and component structure are not a stable API.
Before writing permanent CSS, use the existing opt-in remote-debugging path
against the real authenticated Discord session and record:

- the root application and theme containers;
- semantic background custom properties;
- server/channel/member sidebars;
- chat surface, composer, header, popovers, modals, and settings layers;
- which surfaces are intentionally opaque for readability;
- any shadow roots or constructed stylesheets;
- mutation/remount behavior across channel navigation and theme changes.

The plan intentionally does not guess selectors from public unauthenticated
HTML.

### Styling strategy

The permanent integration follows this order:

1. Make `html`, `body`, and the mounted application root transparent.
2. Override stable semantic theme variables for major surfaces with RGBA tints.
3. Use stable structural or accessibility attributes only when variables cannot
   distinguish a required region.
4. Preserve overlays, menus, dialogs, tooltips, unread markers, and the message
   composer at higher alpha where contrast requires it.
5. Reapply after Discord theme changes without polling every animation frame.
6. Keep a single style element updated in place; do not append new sheets on
   every SPA navigation.

The main chat area should expose the most niri blur. Sidebars and the composer
can retain darker semitransparent tints. Inner CSS `backdrop-filter` may be used
only where it materially improves separation and after verifying it does not
duplicate expensive full-panel blur layers.

### Correctness criteria

- Transparent pixels remain premultiplied and do not produce dark/colored
  fringes around text or rounded corners.
- Text, emoji, animated media, menus, and modals composite correctly.
- Changing Discord light/dark theme updates the glass palette without reload.
- The CSS does not disable reduced-motion/accessibility settings.
- Removing the injected stylesheet restores stock Discord behavior.

## Browser lifecycle and performance policy

### Visible session

Only the visible panel session receives compositor-paced frame opportunities
at the output's presentation interval. Input can request an urgent frame, but
it must still flow through the same one-in-flight acknowledged scheduler.
There is no independent fixed-Hz timer.

Opening Discord immediately presents its retained complete texture, attaches
callbacks, issues one urgent BeginFrame, and then continues from Wayland frame
and presentation feedback. Switching from Discord to Apple Music changes the
visible owner; it does not resize or replace either browser session.

### Hidden session

After the first use, closing Discord:

- clears focus and pointer state;
- calls `WasHidden(true)` after draining/aborting the session's in-flight frame;
- stops that session's frame-opportunity chain;
- retains browser, cookies, WebSocket/service-worker state, history, texture,
  and GPU import cache;
- produces no periodic visual frames by default.

Network and notification state may continue inside Chromium, but hidden visual
work must not run at monitor refresh. If a future unread badge needs updates,
use a low-frequency semantic site event rather than repainting the whole page.

Apple Music keeps its explicit playback-aware parked policy. That exception is
session-local and must not wake Discord.

### Resource ownership

- One process-wide CEF runtime and message pump.
- One persistent Chromium profile shared by the trusted built-in site sessions.
- One frame scheduler, GPU bridge, import cache, and texture per browser view.
- At most one ordinary top-level web panel visible through `PanelManager`.
- A bounded number of presented auxiliary popup sessions.
- Device loss invalidates and rebuilds every session's GPU objects without
  destroying authenticated browser state.

Do not share one `CefGpuFrameBridge` between producers. Similar frame counters
from distinct Viz outputs are not a global identity.

### Loading and failure UX

The shared host tracks enough state to distinguish:

- browser not created;
- loading before first frame;
- usable retained frame;
- main-frame navigation failure;
- renderer recovery in progress;
- fatal required-renderer failure.

Show a small native spinner/message/retry action over the panel only when no
usable frame exists or the session reports a real failure. Do not cover a safe
retained texture merely because a refresh is pending.

## Media identity and background activity

Before Discord can play audio or video, remove the assumption that
`currentProcessChromiumMprisBusName()` means Apple Music.

The preferred design is a narrow per-session site-event path:

- Apple Music's injected integration reports meaningful changes to playback
  state and metadata from its own page;
- the event is tagged by the receiving `CefBrowserSession`, not a user-supplied
  site string;
- `AppleMusicWidget` and the Apple parked-frame policy subscribe to that Apple
  state;
- process-wide Chromium MPRIS remains available to general media UI and can
  represent whichever embedded site owns the active media session;
- the hard-coded `identity = "Apple Music"` rewrite is removed or applied only
  when Apple ownership is independently known.

Do not poll the Apple DOM at frame rate. Install an idempotent page observer and
send changes only when semantic state changes. If the page bridge is temporarily
unavailable, Apple playback controls may fall back to process MPRIS for general
media UI, but the result must not be used to wake the Apple session based on an
unidentified Discord stream.

## Web-platform UX required for Discord

### Navigation and popups

Complete the existing windowless multi-browser phases before allowing a popup.
Every accepted popup gets its own session and Noctalia presenter. Until then,
the current fail-closed `OnBeforePopup()` remains.

Classify navigation explicitly:

- Discord-owned app/auth flows that are safe to embed can use an auxiliary
  windowless sheet;
- ordinary external links open in the user's default browser;
- no request may create a native Chromium Wayland/X11 window;
- closing an auxiliary sheet restores focus to the Discord session.

### Permissions

Discord voice requires an explicit CEF permission path. Add a session-aware
permission handler with origin checks and a native Noctalia decision UI.

- Microphone is requested independently and never silently granted to an
  arbitrary navigated origin.
- Camera and screen capture remain separate decisions.
- A remembered decision is keyed by trusted origin and permission type using
  normal application settings, not a blanket command-line switch.
- Closing the panel does not imply ending an active voice call, but the topbar
  should make ongoing capture visible through existing privacy indicators.
- Screen sharing is accepted only after the Wayland/PipeWire portal path works;
  it must not fall back to X11 capture assumptions.

Core text chat and authentication can land before camera/screen sharing, but
the complete Discord panel acceptance includes microphone voice and hardware-
accelerated video playback.

### File and external operations

Provide deliberate embedder behavior for:

- file chooser/upload;
- downloads and “open externally” actions;
- clipboard read/write requests;
- context menus needed for links, text, and images;
- browser notifications or a future native notification bridge.

These belong to reusable browser-session policy where CEF supplies the event,
with per-site allowlists only where security differs. Do not implement them as
Discord DOM click hacks.

## Implementation sequence

### Phase 1: extract `CefBrowserSession` without changing Apple behavior

Follow Phase 2 of `cef-windowless-multi-browser-plan.md`:

- move all per-browser state and methods out of `CefService::Impl`;
- bind one client and one GPU bridge to the Apple session;
- make `CefSurfaceNode` session-owned;
- retain popup blocking;
- keep Apple rendering, authentication, MPRIS behavior, hover, background
  refresh, fullscreen, and recovery unchanged during the extraction.

Exit criterion: Apple Music behaves identically and no public browser-specific
input/render method remains on `CefService`.

### Phase 2: support multiple top-level sessions

- add live session registration and orderly close/device-rebuild iteration;
- create Apple Music through `createSession()`;
- prove two synthetic sessions cannot cross-route frames, fences, scheduler
  acknowledgements, size, input, cursor, or recovery;
- keep the second synthetic session hidden until its presenter exists.

Exit criterion: two accelerated OSR producers can coexist with independent
state and stable FD/import counts.

### Phase 3: extract the reusable web-panel host

- add `WebPanelSite`, `WebPanelProfile`, and `WebPanel`;
- move common Apple panel rendering/lifecycle code into the host;
- retain `AppleMusicPanel` only for its real specializations;
- change tracing/log labels to use the profile panel ID;
- add shared loading/failure UI;
- generalize floating open-near-click policy through panel virtual behavior
  instead of adding another hard-coded panel-ID branch.

Exit criterion: Apple Music still passes its live acceptance while common code
contains no Apple URL, lyrics, MPRIS, or fullscreen assumptions.

### Phase 4: split site integrations

- move Apple transparent theme and lyrics bridge out of
  `noctalia_cef_app.cpp`;
- add the small origin dispatcher;
- preserve exact Apple script behavior and injection timing;
- unit test trusted-origin matching and main-frame-only injection.

Exit criterion: this refactor alone produces byte-equivalent intended Apple UI
behavior, and the application callback contains no site CSS.

### Phase 5: add the Discord session, panel, and topbar item

- add the Discord profile and session;
- register `discord` with `PanelManager`;
- add the reusable launcher widget and the built-in `discord` widget type;
- expose it in settings with `brand-discord`;
- update translations;
- update the current user's start widget list;
- navigate to `https://discord.com/app` on first open;
- retain the session across close/reopen.

Exit criterion: stock Discord renders and accepts input in a correctly anchored
second panel while Apple remains authenticated and usable.

### Phase 6: implement Discord glass styling

- audit the authenticated runtime DOM and semantic theme variables;
- add the isolated Discord integration;
- make the root transparent and major surfaces semitransparent;
- preserve dialogs, menus, composer contrast, and theme changes;
- verify niri blur through the complete rounded panel and opening animation.

Exit criterion: Discord visually matches the Apple Music glass treatment
without opaque dead regions, unreadable text, blur flicker, or full-root filter
layers.

### Phase 7: decouple Apple identity from process MPRIS

- add the per-session semantic site-event path;
- source Apple widget/background playback decisions from the Apple session;
- remove the unconditional Chromium-to-Apple identity rewrite;
- verify Discord media cannot change Apple widget content or wake its hidden
  renderer.

This phase must land before Discord audio/video is declared supported.

### Phase 8: complete browser UX and permissions

- finish accelerated windowless popup ownership/presentation from the existing
  multi-browser plan;
- implement external navigation, file chooser, and context-menu policy;
- add origin-scoped microphone permission and voice-call validation;
- add camera, notification, and screen-share support in separate focused slices
  as their platform paths become ready.

Exit criterion: Discord login, text chat, uploads, links, and voice work without
native Chromium windows or unsafe blanket permissions.

### Phase 9: performance, recovery, and cleanup

- verify only the visible session receives presentation-paced frames;
- verify hidden Discord produces no visual heartbeat;
- aggregate session memory/FD/import summaries at shutdown without per-frame
  diagnostic logging;
- test renderer termination and Vulkan device rebuild with both sessions;
- remove superseded single-browser APIs and ID-specific PanelManager branches;
- update the multi-browser and zero-copy architecture documents to describe
  multiple top-level sessions.

Exit criterion: the full acceptance list below passes in an optimized build,
with Vulkan validation clean and no runtime renderer fallback.

## Expected repository changes

Likely additions:

- `src/cef/cef_browser_session.h`
- `src/cef/cef_browser_session.cpp`
- `src/cef/site_integrations/site_integration.h`
- `src/cef/site_integrations/site_integration.cpp`
- `src/cef/site_integrations/apple_music_integration.cpp`
- `src/cef/site_integrations/discord_integration.cpp`
- `src/shell/web_panel/web_panel_profile.h`
- `src/shell/web_panel/web_panel_profile.cpp`
- `src/shell/web_panel/web_panel.h`
- `src/shell/web_panel/web_panel.cpp`
- `src/shell/bar/widgets/web_panel_launcher_widget.h`
- `src/shell/bar/widgets/web_panel_launcher_widget.cpp`

Likely modifications:

- `src/cef/cef_service.*`
- `src/cef/cef_surface_node.*`
- `src/cef/noctalia_cef_app.*`
- `src/shell/apple_music/apple_music_panel.*`
- `src/shell/bar/widgets/apple_music_widget.*`
- `src/shell/bar/widget_factory.*`
- `src/shell/settings/widget_settings_registry.cpp`
- `src/shell/panel/panel_manager.*`
- `src/app/application*.cpp`
- `src/dbus/mpris/mpris_service.*`
- `assets/translations/*.json`
- `meson.build`
- focused CEF/session/site-integration tests

Discord itself requires no site-specific Chromium/CEF patch. Multi-session
teardown did expose one genuine transport gap: destroying an offscreen output
backing with a terminal exported read was being reported as a missing release
and losing the GPU context even after the embedder had ended its lease. The
common Viz transport patch now distinguishes that backing-destruction-only
abandonment from a normal frame release. Session extraction, panel hosting,
CSS injection, permissions, and the remaining standard handlers are Noctalia
changes.

## Acceptance criteria

### Isolation and lifecycle

- Apple Music and Discord remain logged in across panel closes and shell
  restarts.
- Opening one panel never navigates, resizes, focuses, or replaces the other.
- Alternating panels shows each retained complete texture immediately.
- Release fences and BeginFrame acknowledgements always return to the producing
  session.
- Closing Discord parks only Discord; playing Apple Music retains its explicit
  background policy.
- Shutdown and graphics-device rebuild drain/recreate both sessions cleanly.

### Discord UX

- The Discord item appears on the left/start side of the configured top bar.
- It opens the panel near the clicked item and never off-screen.
- Login, channel navigation, message composition, selection, clipboard,
  scrolling, scrollbar dragging, and history navigation work.
- A loading or failure state is intentional rather than an empty transparent
  panel.
- External links, popups, uploads, and permission requests have explicit
  behavior.
- Microphone voice works before the feature is called complete; camera and
  screen sharing may be separately staged and clearly reported.

### Visual correctness

- The outer panel matches Apple Music's rounded glass structure.
- Real desktop content below the panel is blurred by niri.
- Discord sidebars/chat/composer retain intentional semitransparent tints.
- Menus, dialogs, tooltips, settings, emoji, text, video, and animated content
  remain legible and correctly composited.
- No opaque root rectangle, transparent seam, dark fringe, or blur flicker is
  visible during open/close or SPA navigation.

### Performance

- The visible session follows exact Wayland presentation feedback rather than
  a 60/120 Hz timer.
- Hidden Discord produces no continuous CEF paints or Noctalia redraws.
- Keeping Discord resident does not cause unbounded renderer, FD, imported
  image, semaphore, or GPU-memory growth.
- Discord hardware video decode uses the already-supported Chromium/VA-API
  pipeline when the media format and driver permit it.
- Apple Music animated artwork remains corruption- and flicker-free while the
  Discord session exists.
- Main-panel and bar responsiveness do not regress while Discord loads or is
  parked.

## Risks and mitigations

### A generic abstraction hides site differences

Keep `WebPanel` limited to rendering/lifecycle mechanics. Apple fullscreen,
Discord permissions, DOM styling, and background policies stay in named site
code. Add a shared feature only after two sites need the same behavior.

### Discord changes its DOM or theme variables

Prefer semantic variables and stable structural attributes, keep one isolated
integration, and make injection idempotent. A site change should break only
Discord styling, not CEF session creation or Apple Music.

### Two heavy web applications increase memory

Create Discord lazily, park visual production when hidden, preserve one
complete texture, and measure optimized-build residency before adding preload
or background refresh. Do not solve first-open latency by making every hidden
site repaint periodically.

### Discord audio hijacks Apple-specific MPRIS behavior

Move Apple ownership to per-session semantic state before accepting Discord
media. Treat Chromium MPRIS as process-wide general media, exactly as it is.

### Authentication or links request native popups

Keep fail-closed blocking until the existing session/popup plan supplies an
accelerated windowless owner. Never relax the Graphite/Dawn/Vulkan-only
contract to make a login flow appear.

### Glass styling reduces contrast

Use differentiated alpha tints, retain higher-opacity menus/composer surfaces,
and validate both Discord themes over bright and dark desktops. Avoid blanket
root opacity.

### Hidden sessions become stale or throttled

Retaining the browser preserves network state; an urgent frame on attachment
updates visual state. Add semantic event bridges for badges/activity rather
than a visual timer. Only a demonstrated real-time background feature may earn
a narrowly scoped exception like Apple Music playback.
