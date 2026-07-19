# CEF Windowless Multi-Browser and Popup Plan

Last updated: 2026-07-18

Status: Phase 1 fail-closed containment implemented; Phases 2–9 planned.

## Decision

Noctalia will remain an exclusively windowless CEF embedder. Every CEF
`WebContents` that can render inside the process must be represented by a
Noctalia-owned browser session with:

- a windowless Alloy browser;
- an exportable offscreen Viz output;
- its own external-BeginFrame scheduler;
- its own `CefGpuFrameBridge`;
- its own generation-safe Graphite texture;
- an explicit Noctalia presentation and input owner.

No Chromium-native X11 or Wayland window may be created inside Noctalia. Page
popups that require browser semantics will become additional windowless
sessions and will be presented by Noctalia. Explicitly external navigation may
be handed to the system browser. Developer tooling will attach to an existing
CEF target instead of creating an unhosted target.

This is a Noctalia browser-lifecycle refactor, not a second rendering project.
The existing per-view CEF/Viz offscreen export transport and the existing
Vulkan/Graphite consumer remain authoritative.

## Why this work is needed

Noctalia currently configures CEF globally for windowless rendering and creates
its primary Apple Music browser with the correct accelerated OSR flags.
However, `CefService::Impl` owns exactly one browser, bridge, texture,
scheduler, size, recovery object, and callback set. `NoctaliaCefClient` does
not implement `OnBeforePopup`.

That leaves a hole outside the primary creation path:

1. a page, CEF API, DevTools command, or remote-debugging endpoint requests a
   second `WebContents`;
2. no Noctalia code gives it a windowless client and presentation owner;
3. CEF/Chromium may create a default native output surface;
4. the Wayland-only Graphite/Dawn build reaches
   `SkiaOutputSurfaceImplOnGpu::InitializeForDawn()` without either an
   offscreen export connection or the unavailable X11 surface path;
5. the debug-check-enabled optimized build terminates the GPU process;
6. repeated GPU recovery can leave the original OSR browser frozen or blank.

This failure was reproduced by creating additional diagnostic Chromium page
targets. The failed GPU processes all stopped in
`viz::SkiaOutputSurfaceImplOnGpu::InitializeForDawn()`. The ordinary primary
Apple Music OSR path was not the source of those extra surfaces.

The immediate safety problem is accidental native-surface creation. The
long-term product problem is that real web popup behavior—including
authentication flows—needs a correct embedded owner rather than blanket
blocking or same-tab redirection.

## Scope

This plan covers:

- separating process-wide CEF runtime state from per-browser state;
- preserving the current primary Apple Music browser behavior;
- intercepting and classifying every popup request;
- creating legitimate popups as accelerated windowless sessions;
- presenting popup sessions inside the Apple Music panel;
- routing frames, presentation opportunities, input, focus, cursor, and close
  requests to the correct session;
- handling multiple sessions during graphics-device recovery and shutdown;
- preventing developer tooling from creating unhosted render targets;
- making an accidentally reached native Dawn/Wayland path fail gracefully
  instead of killing the GPU process;
- focused unit, CEF, Vulkan-validation, and live acceptance coverage.

## Terminology

CEF uses “popup” for two different mechanisms:

- A **browser popup** is a new `CefBrowser` requested through
  `CefLifeSpanHandler::OnBeforePopup()`. Examples include `window.open()`,
  `target="_blank"`, authentication windows, and new-window dispositions.
  These require the additional browser sessions described by this plan.
- An **OSR widget popup** is transient content such as a `<select>` menu,
  reported through `CefRenderHandler::OnPopupShow()`,
  `OnPopupSize()`, and `PET_POPUP` paint callbacks. It is part of one existing
  browser and must not be registered as another `CefBrowserSession`.

The current accelerated callback accepts only `PET_VIEW`. Correct accelerated
OSR widget-popup composition is related rendering work, but it is not the cause
of the native-output crash and must remain a separate, focused slice. The
multi-browser refactor must not accidentally route `PET_POPUP` frames into a
browser-session bridge as though they were complete roots.

## Non-goals

This work will not:

- implement native Chromium window decorations or Wayland WSI;
- add an X11 backend or fallback;
- introduce a CPU CEF paint path;
- add Ganesh, GLES, or non-Vulkan fallbacks;
- turn Noctalia into a general tabbed browser;
- redirect every popup into the primary page;
- open every popup in an external browser;
- add versioned protocols, runtime capability negotiation, schema validation,
  or build-contract ceremony for code that is built together;
- broaden the Chromium patch stack with application-specific naming or
  temporary diagnostics.

Native on-screen Graphite/Dawn/Wayland is a separate Nucleus Browser concern.
Noctalia owns its visible Wayland surfaces and only needs accelerated OSR
content.

## Current architecture and exact gap

### What already exists

The current stack already provides:

- `CefSettings.windowless_rendering_enabled`;
- an Alloy OSR browser created with `SetAsWindowless(0)`;
- mandatory `shared_texture_enabled` and
  `external_begin_frame_enabled`;
- transparent browser output;
- per-`CefRenderWidgetHostViewOSR` offscreen-output Mojo endpoints;
- Graphite/Dawn/Vulkan rendering in the CEF GPU process;
- DMA-BUF and producer-fence delivery through `OnAcceleratedPaint`;
- direct Graphite sampling with token-correlated release fences;
- compositor-paced external BeginFrames;
- persistent cookies, cache, authentication, audio, and browser history;
- input, focus, cursor, resize, fractional scaling, and renderer recovery.

The CEF patch creates the offscreen endpoint pair from the individual OSR
render-widget view. It is not inherently limited to one browser. A second
properly configured OSR browser can receive its own producer queue and
transport epoch.

### What is single-browser today

`CefService::Impl` currently contains one instance of each of the following:

- `CefRefPtr<CefBrowser> browser`;
- `CefRefPtr<CefClient> client`;
- `std::unique_ptr<CefGpuFrameBridge> gpuBridge`;
- `TextureHandle texture`;
- `CefExternalFrameScheduler frameScheduler`;
- BeginFrame watchdog and renderer-recovery state;
- logical size, device scale, attachment state, and pending URL;
- frame-ready, frame-opportunity, and cursor callbacks;
- pointer coalescing, pressed-button state, and cursor state.

`OnAfterCreated()` assigns its argument directly to the one browser field.
`OnAcceleratedPaint()` sends every frame to the one bridge. The bridge's
release callback sends every fence through the one browser host.
`CefSurfaceNode` refers to the service rather than a particular browser.
`AppleMusicPanel` consequently hosts exactly one texture and one input target.

Allowing a second browser with this structure would cause state aliasing even
if it remained windowless:

- a popup could overwrite the primary browser pointer;
- frames from two producers could enter one import cache;
- equal per-view capture counters could be returned to the wrong host;
- one scheduler could acknowledge or abort the other view's BeginFrame;
- size, scale, focus, cursor, recovery, and close events could affect the wrong
  browser.

Therefore merely adding `windowInfo.SetAsWindowless(0)` to `OnBeforePopup`
would not be sufficient. Until the per-session ownership exists, the safe
containment is to reject popup creation explicitly.

## Required invariants

The implementation is complete only when all of these are structural
invariants:

1. **No implicit browser creation:** every browser is created or adopted by one
   `CefBrowserSession`.
2. **No native output:** every renderable browser uses windowless Alloy OSR,
   shared textures, external BeginFrames, and the exportable offscreen output.
3. **One producer, one bridge:** frames and release fences never cross session
   boundaries.
4. **One scheduler transaction domain:** BeginFrame request IDs, generations,
   acknowledgements, aborts, and watchdogs are session-local.
5. **One UI owner per visible session:** every visible texture has a scene
   node, bounds, input routing, and a compositor frame-opportunity source.
6. **Correct popup semantics:** embedded popups retain the opener relationship,
   request context, cookies, JavaScript access, and close behavior supplied by
   CEF.
7. **Explicit hidden behavior:** a session without a visible UI owner is
   `WasHidden(true)` and cannot free-run rendering.
8. **Balanced frame lifetime:** close, hide, replacement, device loss, and
   shutdown either return the correct GPU release fence or explicitly abandon
   only device-lost resources.
9. **Failure isolation:** failure to create one auxiliary browser cannot
   replace, close, or corrupt the primary browser.
10. **No fatal escape hatch:** accidentally requesting an unsupported native
    Dawn/Wayland surface produces a named error and terminates that creation
    path, not the process-wide GPU service.

These extend rather than replace the frame and BeginFrame invariants in
`cef-zero-copy-frame-architecture.md`.

## Target architecture

```text
Application
  |
  +-- CefService                         process-wide
  |     - CefInitialize / CefShutdown
  |     - external message pump
  |     - persistent request context
  |     - GraphicsDevice binding
  |     - live browser registry
  |     - pending popup registry
  |
  +-- CefBrowserSession (primary)        one browser/view
  |     - CefBrowser + session client
  |     - dimensions / scale / visibility
  |     - BeginFrame scheduler/watchdog
  |     - renderer recovery
  |     - CefGpuFrameBridge
  |     - TextureHandle
  |     - input/cursor state
  |
  +-- CefBrowserSession (popup)          one browser/view
        - same per-view state
        - weak parent relationship
        - popup presentation metadata

AppleMusicPanel
  |
  +-- CefBrowserHostNode
        - primary CefSurfaceNode
        - zero or more popup CefSurfaceNodes
        - topmost focus/input routing
        - one Wayland frame callback wakes all visible sessions
```

### `CefService`: process-wide runtime

`CefService` remains the only owner of CEF initialization and shutdown. It
should retain:

- CEF distribution/helper paths;
- `NoctaliaCefApp`;
- global initialization state;
- the external-message-pump scheduling bridge;
- the process-wide `GraphicsDevice` and `GraphiteTextureManager` attachment;
- a registry from CEF browser ID to live session;
- pending popup creations keyed by `(opener browser ID, popup ID)`;
- the primary session;
- orderly close/drain of all sessions.

It should no longer expose browser-specific navigation, input, sizing, texture,
or presentation methods. Those move to `CefBrowserSession`.

The registry uses CEF's existing integer browser identifier internally. UI
code holds `std::shared_ptr<CefBrowserSession>` directly; no new public
versioned handle protocol is needed.

### `CefBrowserSession`: per-view state

Introduce a CEF-free public class with a private implementation:

```cpp
class CefBrowserSession {
public:
  void ensureBrowser(int logicalWidth, int logicalHeight);
  void navigate(const std::string& url);
  void resize(int logicalWidth, int logicalHeight);
  void setDeviceScale(float scale);
  void setDisplayAttached(bool attached);

  void sendMouseMove(...);
  void sendMouseButton(...);
  void sendMouseWheel(...);
  void sendKey(...);
  void setFocus(bool focused);
  void goBack();
  void goForward();
  void close();

  bool onFrameOpportunity();
  void onPresentation(const SurfacePresentationFeedback&);
  bool uploadIfDirty(TextureManager&);
  TextureHandle currentTexture() const noexcept;

  void setFrameReadyCallback(...);
  void setFrameOpportunityCallback(...);
  void setCursorCallback(...);
};
```

Its implementation owns everything currently in `CefService::Impl` that can
differ per browser:

- browser and per-session client;
- pending initial URL;
- size, scale, visibility, and focus;
- bridge, texture, and dirty state;
- frame scheduler, watchdog, and recovery;
- pointer coalescing and pressed-button state;
- callbacks and cursor state;
- weak parent session for auxiliary browsers;
- popup bounds/disposition metadata;
- a session-local alive token for deferred callbacks.

The class remains free of CEF types in its public header, matching the current
service boundary.

### Client ownership and callback dispatch

Use one `NoctaliaCefClient` instance per session. The client holds a reference
to its session implementation and dispatches all callbacks directly to that
session. Do not keep one shared client that looks sessions up after every
callback.

The session client must still verify that callback browser IDs match its bound
browser after `OnAfterCreated()`. A mismatch is a local protocol error and
must not fall back to the primary session.

This makes routing unambiguous for:

- accelerated frames;
- release-fence return;
- BeginFrame completion;
- load state and renderer termination;
- cursor and focus;
- popup creation and close.

### Primary session

Application startup creates one persistent primary session through
`CefService`. `AppleMusicPanel` retains that session and passes it to
`CefSurfaceNode`.

Closing the visible panel continues to detach rather than destroy the primary
session, preserving audio, authentication, history, and instant reopen.

The first extraction must be behavior-preserving: before popup support is
enabled, the primary session should render and interact exactly as the current
single `CefService`.

## Popup creation contract

### Interception

`NoctaliaCefClient::OnBeforePopup()` must handle every callback. It may never
return `false` while leaving `windowInfo` at its default native configuration.

For a popup that Noctalia accepts:

1. Create a pending `CefBrowserSession` associated with the opener session and
   CEF `popup_id`.
2. Create a per-session `NoctaliaCefClient` for it.
3. Call `windowInfo.SetAsWindowless(0)`.
4. Set `windowInfo.shared_texture_enabled = 1`.
5. Set `windowInfo.external_begin_frame_enabled = 1`.
6. Set the transparent browser background and the current compositor-derived
   windowless frame-rate ceiling.
7. Assign the pending session's client through the `client` parameter.
8. Preserve CEF's request context and JavaScript opener access; do not force
   `no_javascript_access`.
9. Record the requested disposition and popup features for Noctalia's
   presenter.
10. Return `false` to allow CEF to create that explicitly windowless browser.

`extra_info` is not needed as a browser-process correlation protocol. The
dedicated client already carries the pending session. Avoid adding redundant
IDs to renderer-process data.

### Asynchronous correlation

CEF popup creation has three terminal paths:

- `OnAfterCreated()` binds the new `CefBrowser` to its pending session, moves
  it into the live registry, and notifies the presenter.
- `OnBeforePopupAborted()` removes the pending record identified by opener
  browser ID and popup ID.
- closing the opener removes any still-pending descendants if neither callback
  can produce a usable browser.

The pending registry is required by CEF's lifecycle contract; it is not a
general protocol or capability system.

### Presentation policy

For the first complete implementation, every accepted browser popup is shown
as a Noctalia-owned child sheet over the Apple Music panel. This deliberately
avoids inventing a tab system while preserving web behavior.

- `NEW_POPUP`, `NEW_WINDOW`, and foreground-tab dispositions become a visible
  child sheet.
- A requested size is clamped to the Apple Music content bounds.
- Missing or unusable dimensions use a centered, comfortably sized sheet.
- The sheet receives Noctalia-drawn chrome only where needed for close and
  navigation safety; CEF never draws native decorations.
- JavaScript `window.close()` removes the corresponding sheet.
- The close affordance calls `CloseBrowser(true)` on that session.
- Nested popups form a small presentation stack. Only the topmost sheet
  receives keyboard focus and pointer input.
- Closing a parent closes its descendants before removing the parent session.

Background-tab disposition should initially create a hidden windowless session
and surface a lightweight “opened page” affordance in the popup stack. It must
not render continuously while hidden. If no product UI consumes background
tabs, the final implementation may instead reject that disposition explicitly;
it must not create an ownerless browser.

### External browser policy

Opening externally is an explicit action, not a URL-based automatic fallback.
Moving an authentication or script-opened popup to another browser would lose
`window.opener`, the CEF request context, and JavaScript communication.

Noctalia may offer “Open in default browser” for ordinary links or popup
content. When selected, it launches the URL externally and closes or cancels
the embedded auxiliary session as appropriate. The external launcher must use
the existing process-launching abstraction rather than execute a shell command
constructed from a URL.

### Temporary fail-closed stage

Before multi-session ownership lands, `OnBeforePopup()` should return `true`
for all requests after logging one concise rejection without the target URL
(which may contain sensitive query data). This is now implemented. It is
acceptable only as a short-lived safety stage: it prevents another
native-surface crash but is not the final behavior because it can break
authentication and legitimate web flows.

## Scene and presentation integration

### Session-bound surface node

Refactor `CefSurfaceNode` to accept a `CefBrowserSession&` or shared session
reference instead of `CefService&`. Its rendering and input code otherwise
remains largely unchanged.

Each node:

- adopts only its session's current texture;
- forwards input only to that session;
- applies the session's cursor to its own input area;
- attaches and detaches only that session's callbacks;
- invalidates only that session on device rebuild.

### Browser host node

Add a small Apple Music browser host node or equivalent panel-owned container:

- primary surface fills the rounded panel content bounds;
- popup surfaces are layered above it;
- the topmost popup establishes focus and pointer priority;
- panel background and clipping remain Noctalia/Graphite operations;
- removing a popup cannot rebuild or detach the primary surface.

This host is not a generic browser UI framework. It is the minimum presentation
owner required for the sessions CEF is allowed to create.

### Shared Wayland frame clock

Multiple sessions presented on one `wl_surface` do not need independent
Wayland callback chains. `AppleMusicPanel` should keep one compositor callback
chain and offer each visible session the same frame opportunity:

1. the panel receives `wl_surface.frame`;
2. it calls `onFrameOpportunity()` on each visible session;
3. any session needing another opportunity keeps the panel callback armed;
4. a fresh frame from any session requests scene update and redraw;
5. presentation feedback is forwarded to every visible session using that
   surface.

Each session retains its own one-in-flight Chromium BeginFrame transaction.
The shared Wayland clock does not merge scheduler request IDs or
acknowledgements.

Hidden sessions receive `WasHidden(true)`, stop external frame opportunities,
and drain any already accepted BeginFrame according to the existing exact-once
contract. Audio behavior remains governed by Chromium and is not coupled to
painting.

## Input, focus, and cursor

The panel host owns routing:

- pointer hit testing selects the topmost visible session under the pointer;
- a press focuses that session before forwarding the event;
- held-button motion remains routed to the pressed session until release so
  dragging does not jump between layers;
- keyboard events go only to the focused session;
- closing the focused popup restores focus to its parent;
- history buttons affect the focused session;
- cursor changes are stored per session and applied only while its input area
  is active.

The existing raw-Alt suppression and history-shortcut handling move with the
session-bound input node. They must not become process-wide state.

## Frame transport and GPU ownership

No changes are required to the core zero-copy protocol. Each session simply
owns an independent instance of it.

For every session:

- the CEF view owns its own offscreen output queue and transport epoch;
- `OnAcceleratedPaint(browser, ...)` reaches only that browser's client and
  bridge;
- import identity is scoped to that bridge;
- the stable `TextureHandle` belongs to that session;
- the bridge's release callback captures a weak session reference and returns
  the fence through that exact session's `CefBrowserHost`;
- closing one session drains or discards only its pending frame;
- per-session statistics are logged at session close and may be summed once at
  process shutdown.

The bridge must not be moved into `CefService` as a shared multi-producer
cache. Separate bridges avoid capture-counter, transport-epoch, queue-state,
and texture-lifetime aliasing.

Multiple bridges may share:

- the process-wide `GraphicsDevice`;
- the graphics queue;
- the Graphite recorder;
- `GraphiteTextureManager`;
- allocator infrastructure.

Graphite submission dependencies already serialize through the common
renderer. They remain tied to the specific external image being sampled.

## Graphics-device loss and renderer recovery

### Noctalia Vulkan device loss

`CefService::prepareForGraphicsDeviceRebuild()` must iterate all live and
pending sessions:

1. stop new session BeginFrames;
2. abandon each device-local bridge without submitting to the lost queue;
3. invalidate every session texture;
4. rebuild the process-wide Vulkan/Graphite device;
5. create a fresh bridge for every still-live session;
6. request a fresh frame from each visible session;
7. leave hidden sessions invalid until reattached.

Stale texture generations remain protected by the existing texture-manager
contract.

### CEF renderer termination

Renderer recovery is session-local. A terminated popup may reload or close
without replacing the primary browser. A primary renderer termination follows
the existing bounded recovery behavior.

A process-wide CEF GPU-process failure can affect every browser, but recovery
callbacks must still be dispatched to their own sessions. No session may
assign its recovered render view to another session.

### Offscreen-output terminal failure

An export protocol, allocation, context, or synchronization failure retires
only the affected output generation. Existing CEF recovery creates new
per-view endpoints. If recovery is exhausted, close the auxiliary session or
show the primary panel's renderer error; never create a native or CPU fallback.

## Shutdown

Shutdown must be registry-driven rather than waiting on one browser pointer:

1. stop accepting popup creation;
2. disable cross-thread external-message-pump scheduling;
3. stop all session frame opportunities;
4. close auxiliary descendants from deepest to shallowest;
5. close the primary browser;
6. pump CEF until every live and pending session reaches its terminal callback
   or the existing bounded shutdown deadline expires;
7. log any browser IDs that failed to close;
8. release every bridge and session client;
9. clear the registry;
10. call `CefShutdown`.

`OnBeforeClose()` removes exactly its browser from the live registry. It must
not null the primary session merely because an auxiliary browser closed.

## Developer tooling

Production builds do not expose a remote-debugging port.

For deliberate diagnostics:

- enable remote debugging only through an explicit test invocation;
- query the target list and attach an external Chrome DevTools frontend to the
  existing Apple Music target;
- do not call `/json/new`;
- do not use `ShowDevTools()` until it is routed through a windowless session;
- do not create `chrome://` diagnostic targets inside Noctalia merely to read
  their state;
- collect needed Chromium state through logs, tracing, or the existing target's
  DevTools protocol.

If in-process DevTools is later desired, `OnBeforeDevToolsPopup()` must create
a dedicated windowless session and present it through the same host. It is not
a special native-window exception.

The remote-debugging server's arbitrary new-target endpoint is outside the
supported Noctalia contract. Reaching it must still be contained by the
Chromium hardening below rather than crashing the GPU process.

## Chromium/CEF hardening

Noctalia-side interception is the primary guarantee. Add defense in depth
inside the existing generic CEF/Chromium patch ownership:

1. In the Graphite/Dawn output initialization already modified by
   `0001-viz-offscreen-output-transport.patch`, replace the unsupported
   Linux native-surface `NOTREACHED()` path with a named error and a graceful
   `false` return.
2. Verify the failed surface creation is isolated and does not trigger a
   repeated GPU-process restart loop.
3. Keep the exportable-offscreen check strict: an export connection requires
   Graphite/Dawn/Vulkan and must never fall back to Ganesh, GL, or an ordinary
   offscreen surface.
4. Add a focused test that requests an unsupported non-offscreen surface in
   the Wayland-only configuration and proves that it fails without a fatal
   check.
5. Add or extend the OSR popup test so two windowless browsers create distinct
   offscreen-output endpoint pairs.

Because patch `0001` already owns `InitializeForDawn()` and the offscreen
connection routing, this hardening belongs there. Do not add a second patch
that edits the same area.

CEF API behavior specific to OSR popup creation belongs in the existing CEF
OSR patch only if upstream CEF fails to create independent endpoints for a
properly configured windowless popup. Current source structure indicates that
the endpoints are per `CefRenderWidgetHostViewOSR`, so no speculative producer
patch should be added.

## Implementation sequence

### Phase 1: fail closed and lock the creation boundary — complete

Implement the smallest immediate safety change:

- add `OnBeforePopup()` to the current client;
- cancel all popups explicitly;
- add one concise log with disposition, popup ID, opener ID, and user-gesture
  state, without recording the potentially sensitive target URL;
- ensure production remote debugging remains disabled;
- document that this is temporary compatibility containment.

Acceptance:

- `window.open()` and `target="_blank"` cannot create a native surface;
- requesting a popup does not alter the primary browser;
- Apple Music primary rendering, playback, and navigation remain unchanged.

This phase requires only a Noctalia rebuild.

### Phase 2: extract the primary `CefBrowserSession`

Perform a behavior-preserving refactor:

- add `cef_browser_session.h/.cpp`;
- move browser-specific state and methods out of `CefService::Impl`;
- give the primary browser a dedicated client bound to the session;
- change `CefSurfaceNode` and `AppleMusicPanel` to use the primary session;
- keep popup creation canceled;
- keep process initialization and the message pump in `CefService`.

Acceptance:

- the public service no longer exposes browser-specific input/render methods;
- Apple Music retains profile, history, authentication, audio, rendering,
  timing, input, cursor, and close/reopen behavior;
- one session produces the same frame-bridge statistics as before;
- no CEF or Chromium rebuild is required.

### Phase 3: add live and pending session ownership

Add the minimum lifecycle registry:

- live sessions keyed by CEF browser ID;
- pending popup sessions keyed by opener browser ID and popup ID;
- per-session clients;
- `OnAfterCreated`, `OnBeforePopupAborted`, and `OnBeforeClose` transitions;
- parent/descendant close ordering;
- session-local release callbacks and recovery.

Keep accepted popup presentation disabled until a UI owner exists. Tests may
create and immediately close a hidden popup session.

Acceptance:

- two synthetic sessions cannot exchange frames, release fences, scheduler
  acknowledgements, cursor state, or close callbacks;
- aborting a pending popup leaks neither a client nor a session;
- closing a popup leaves the primary registered and usable.

### Phase 4: create accelerated windowless popups

Replace the temporary cancellation with explicit OSR creation:

- configure `windowInfo` and browser settings as described above;
- supply the pending session client;
- preserve opener/request-context semantics;
- begin hidden and call `WasHidden(true)` until the presenter adopts it;
- reject a creation if the graphics bridge is unavailable.

Acceptance:

- a local `window.open()` fixture creates a second OSR browser;
- both browsers receive accelerated frames from distinct output generations;
- neither browser invokes CPU `OnPaint`;
- JavaScript communication through `window.opener` works;
- `window.close()` reaches the correct session.

### Phase 5: add the Apple Music popup presenter

Add the panel-owned browser host:

- render primary and popup session nodes;
- implement bounded sheet geometry and clipping;
- route focus, pointer capture, keyboard, cursor, history, and close;
- drive all visible sessions from the panel's Wayland frame clock;
- hide background sessions;
- restore parent focus after close.

Acceptance:

- authentication-style popup fixture is fully interactive;
- nested popup close order is correct;
- scrollbar dragging and text input work in the popup;
- primary audio and state survive popup open/close;
- panel close/reopen restores the same session stack or intentionally closes
  transient popups according to one documented policy.

Default policy: close transient auxiliary sessions when the Apple Music panel
is explicitly closed, while retaining the primary session and audio. This
avoids invisible authentication windows and orphaned popup UI.

### Phase 6: multi-session recovery and shutdown

Generalize:

- graphics-device loss;
- CEF renderer recovery;
- offscreen-output recovery;
- panel detach drains;
- bounded process shutdown;
- aggregate health logging.

Acceptance:

- forced device rebuild invalidates every old texture generation and resumes
  every visible session;
- terminating one popup renderer does not reload the primary;
- closing with multiple sessions drains all `OnBeforeClose` callbacks;
- FD, imported-image, semaphore, and session counts return to baseline.

### Phase 7: developer-tooling safety

Make the supported workflow explicit and automated where useful:

- add a documented command that enables remote debugging for a test run;
- connect tooling to the existing target;
- make accidental new-target creation visibly unsupported;
- optionally add windowless DevTools presentation only if it has a real
  product/debugging need.

Acceptance:

- inspecting Apple Music does not create an additional Chromium output
  surface;
- accidental unhosted target creation cannot terminate the GPU process.

### Phase 8: Chromium hardening and focused CEF tests

Update the existing patch rather than adding overlap:

- make unsupported native Dawn/Wayland initialization non-fatal;
- add the focused surface-failure test;
- prove distinct offscreen endpoint creation for multiple OSR browsers;
- regenerate CEF API artifacts only if an API change is actually required.

Acceptance:

- focused Chromium/Viz and CEF tests pass;
- the patch stack still has one owner per modified source area;
- the optimized CEF build completes with the existing Graphite/Dawn/Vulkan
  contract.

### Phase 9: full runtime acceptance

Build and install CEF only if Phase 8 changed it, then rebuild Noctalia and run:

- ordinary Apple Music use;
- page popup and nested-popup fixtures;
- Apple Music authentication and external links;
- animated artwork and simultaneous popup animation;
- resize and fractional scaling;
- panel close/reopen;
- popup close during an in-flight frame;
- Vulkan device recovery;
- CEF renderer recovery;
- shutdown with a popup open;
- Vulkan synchronization validation.

Do not add runtime fallbacks in response to a failed test. Correct the owning
session or producer contract.

## Expected file changes

### Noctalia

Likely additions:

- `src/cef/cef_browser_session.h`
- `src/cef/cef_browser_session.cpp`
- `src/shell/apple_music/apple_music_browser_host_node.h`
- `src/shell/apple_music/apple_music_browser_host_node.cpp`
- focused session/popup lifecycle tests

Likely modifications:

- `src/cef/cef_service.h`
- `src/cef/cef_service.cpp`
- `src/cef/cef_surface_node.h`
- `src/cef/cef_surface_node.cpp`
- `src/shell/apple_music/apple_music_panel.h`
- `src/shell/apple_music/apple_music_panel.cpp`
- `src/app/application_services.cpp`
- `src/app/application_ui.cpp`
- relevant Meson source/test lists

The current `NoctaliaCefClient` may remain private to a CEF `.cpp`, but it
should be moved out of the already large service implementation if doing so
makes session ownership clearer. Do not create separate policy, factory, and
registry classes unless the implementation demonstrates an actual independent
responsibility.

### Nucleus CEF patch stack

Expected modification:

- `cef/patches/0001-viz-offscreen-output-transport.patch`

Possible modification, only if a proven CEF OSR limitation requires it:

- `cef/patches/0002-cef-osr-dmabuf-and-frame-scheduling.patch`

Do not add Noctalia-specific names to either patch.

## Test design

### Unit tests without CEF

Extract enough lifecycle logic to test:

- pending popup accepted, created, aborted, and opener-closed transitions;
- duplicate or unknown browser IDs;
- parent/descendant close ordering;
- focused-session selection;
- one shared frame opportunity offered to multiple visible schedulers;
- hidden sessions not sustaining repaint;
- device-rebuild iteration over live sessions.

Do not mock CEF's entire API. Test Noctalia-owned state transitions and use
focused CEF integration tests for CEF behavior.

### Local deterministic web fixtures

Use small local pages for:

- `window.open()` with opener-to-child and child-to-opener messaging;
- `target="_blank"` with a user gesture;
- automatic popup rejection;
- requested popup dimensions;
- nested popup and JavaScript close;
- simultaneous animation in parent and child;
- text input, pointer capture, wheel scrolling, and scrollbar dragging;
- transparent popup content;
- renderer termination during popup activity.

These fixtures should not depend on Apple Music or network timing.

### CEF/Chromium focused tests

Cover:

- two OSR views receiving distinct offscreen endpoint pairs;
- release of one view not advancing the other output queue;
- popup output initialization selecting Graphite/Dawn/Vulkan export;
- unsupported native Wayland output returning failure without a fatal check;
- closing a popup with a published frame retires its output generation cleanly.

### Live acceptance

The release is accepted only when:

- Apple Music remains visually and audibly correct;
- any real popup used by authentication is visible and interactive;
- no native CEF window appears;
- no CPU `OnPaint` occurs;
- no Graphite/Dawn output silently becomes Ganesh or GL;
- continuous animation remains compositor-paced;
- the primary page never freezes or blanks after popup/tooling activity;
- validation reports no ownership, layout, semaphore, or lifetime errors;
- repeated popup cycles keep browser, FD, import, and memory counts bounded.

## Risks and controls

### Popup created before UI adoption

Create the session hidden. Do not issue external BeginFrames until the panel
presenter attaches it.

### Opener closes during asynchronous creation

Remove the pending record and close or abort the child. Never promote it to an
ownerless live session.

### Multiple sessions overload one frame callback

One Wayland opportunity may drive multiple session schedulers, but each
scheduler permits only one outstanding request. If total rendering exceeds the
refresh budget, Chromium naturally backpressures per session; do not add
free-running timers.

### Auxiliary browser consumes unbounded resources

Transient popups close with the panel and descendants close with parents.
Rely on Chromium popup blocking plus explicit UI ownership. Add a small maximum
only if a concrete site can create unbounded accepted sessions; do not start
with a configurable policy framework.

### External link breaks authentication

Do not auto-externalize based on hostname or disposition. Preserve embedded
popup semantics unless the user explicitly chooses external navigation.

### Graceful Dawn failure still triggers recovery loops

Test the exact error propagation before declaring the Chromium hardening
complete. If returning `false` still causes repeated GPU restarts, reject the
unhosted surface earlier in the CEF browser-creation path while retaining the
non-fatal Viz guard.

## Completion criteria

This work is complete when:

1. the primary browser is one ordinary `CefBrowserSession`, not special state
   embedded in `CefService`;
2. all popup-capable CEF callbacks are handled explicitly;
3. legitimate popups are accelerated OSR sessions with Noctalia UI ownership;
4. every frame, fence, scheduler acknowledgement, cursor, and close callback
   is routed to exactly one session;
5. no CEF operation can implicitly create a native Chromium surface inside
   Noctalia;
6. an accidental unsupported target cannot kill the CEF GPU process;
7. device recovery and shutdown work with multiple sessions;
8. the existing Apple Music experience and zero-copy frame contract remain
   correct and performant.

The architectural result is intentionally narrow: one robust windowless CEF
runtime capable of hosting the primary Apple Music page and the auxiliary
browser sessions that real web behavior requires.
