#include "cef/cef_service.h"

#include "cef/cef_browser_session.h"
#include "cef/cef_external_frame_scheduler.h"
#include "cef/cef_gpu_frame_bridge.h"
#include "cef/cef_pointer_motion_coalescer.h"
#include "cef/cef_renderer_recovery.h"
#include "cef/noctalia_cef_app.h"
#include "cef/site_integrations/site_integration_policy.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/timer_manager.h"
#include "core/tracy.h"
#include "core/tracy_latency.h"
#include "cursor-shape-v1-client-protocol.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_permission_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_helpers.h"
#include "render/core/texture_manager.h"
#include "render/graphics_device.h"
#include "ui/dialogs/file_dialog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("cef");
  constexpr int kCefWindowlessFrameRate = 120;
  constexpr std::int64_t kFallbackBeginFrameIntervalNs = 1'000'000'000LL / kCefWindowlessFrameRate;
  constexpr std::int64_t kBackgroundBeginFrameIntervalNs = 1'000'000'000LL;
  constexpr auto kBackgroundBeginFrameInterval = std::chrono::milliseconds(1000);
  constexpr std::int64_t kBeginFrameAckRecoveryNs = 2'000'000'000;
  constexpr auto kPresentationResizeCaptureTimeout = std::chrono::milliseconds(500);

  std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // Resolve the CEF subprocess helper binary next to the running executable.
  std::string helperNextToSelf() {
    std::string exe(PATH_MAX, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", exe.data(), exe.size());
    if (n <= 0) {
      return {};
    }
    exe.resize(static_cast<std::size_t>(n));
    const auto slash = exe.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : exe.substr(0, slash);
    return dir + "/noctalia_cef_helper";
  }

  std::string userCachePath() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/noctalia/cef";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.cache/noctalia/cef";
    }
    return "/tmp/noctalia-cef";
  }

  std::uint32_t cefModifiersFromKeyMod(std::uint32_t mods) {
    std::uint32_t flags = 0;
    if (mods & KeyMod::Shift) {
      flags |= EVENTFLAG_SHIFT_DOWN;
    }
    if (mods & KeyMod::Ctrl) {
      flags |= EVENTFLAG_CONTROL_DOWN;
    }
    if (mods & KeyMod::Alt) {
      flags |= EVENTFLAG_ALT_DOWN;
    }
    if (mods & KeyMod::Super) {
      flags |= EVENTFLAG_COMMAND_DOWN;
    }
    return flags;
  }

  std::uint32_t cefButtonFlag(int button) {
    if (button == 1) {
      return EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    }
    if (button == 2) {
      return EVENTFLAG_RIGHT_MOUSE_BUTTON;
    }
    return EVENTFLAG_LEFT_MOUSE_BUTTON;
  }

  // XKB keysym -> Windows virtual-key code for the non-printable keys the browser
  // needs for navigation/editing. Printable keys ride in on the CHAR event.
  int windowsKeyCodeFromSym(std::uint32_t sym) {
    switch (sym) {
    case 0xff08:
      return 0x08; // BackSpace -> VK_BACK
    case 0xff09:
      return 0x09; // Tab -> VK_TAB
    case 0xff0d:
      return 0x0D; // Return -> VK_RETURN
    case 0xff1b:
      return 0x1B; // Escape -> VK_ESCAPE
    case 0xff50:
      return 0x24; // Home
    case 0xff51:
      return 0x25; // Left
    case 0xff52:
      return 0x26; // Up
    case 0xff53:
      return 0x27; // Right
    case 0xff54:
      return 0x28; // Down
    case 0xff55:
      return 0x21; // Page_Up
    case 0xff56:
      return 0x22; // Page_Down
    case 0xff57:
      return 0x23; // End
    case 0xffff:
      return 0x2E; // Delete
    default:
      break;
    }
    // ASCII letters/digits map directly to their uppercase VK code.
    if (sym >= '0' && sym <= '9') {
      return static_cast<int>(sym);
    }
    if (sym >= 'a' && sym <= 'z') {
      return static_cast<int>(sym - ('a' - 'A'));
    }
    if (sym >= 'A' && sym <= 'Z') {
      return static_cast<int>(sym);
    }
    return 0;
  }

  // CEF shared-texture pixel format -> DRM FourCC (little-endian byte order).
  std::uint32_t drmFourccFromCef(cef_color_type_t type) {
    const auto fourcc = [](char a, char b, char c, char d) {
      return static_cast<std::uint32_t>(a)
          | (static_cast<std::uint32_t>(b) << 8)
          | (static_cast<std::uint32_t>(c) << 16)
          | (static_cast<std::uint32_t>(d) << 24);
    };
    switch (type) {
    case CEF_COLOR_TYPE_RGBA_8888:
      return fourcc('A', 'B', '2', '4'); // DRM_FORMAT_ABGR8888 (bytes R,G,B,A)
    case CEF_COLOR_TYPE_BGRA_8888:
    default:
      return fourcc('A', 'R', '2', '4'); // DRM_FORMAT_ARGB8888 (bytes B,G,R,A)
    }
  }

  std::uint32_t cursorShapeFromCef(cef_cursor_type_t type) {
    switch (type) {
    case CT_HAND:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    case CT_IBEAM:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    case CT_CROSS:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    case CT_WAIT:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
    case CT_EASTWESTRESIZE:
    case CT_COLUMNRESIZE:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
    case CT_NORTHSOUTHRESIZE:
    case CT_ROWRESIZE:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
    case CT_NOTALLOWED:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
    case CT_GRAB:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
    case CT_GRABBING:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
    case CT_POINTER:
    default:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
  }

  bool canOpenExternally(std::string_view url) noexcept {
    return url.starts_with("https://") || url.starts_with("http://") || url.starts_with("mailto:");
  }

  bool launchExternalUrl(std::string_view sessionId, std::string_view url) {
    if (!canOpenExternally(url)) {
      return false;
    }
    if (process::runAsync(std::vector<std::string>{"xdg-open", std::string(url)})) {
      kLog.info("opened external URL from CEF session '{}' in the default browser", sessionId);
      return true;
    }
    kLog.error("failed to open external URL from CEF session '{}'", sessionId);
    return false;
  }

} // namespace

class NoctaliaExternalBeginFrameCallback : public CefExternalBeginFrameCallback {
public:
  NoctaliaExternalBeginFrameCallback(
      CefBrowserSession::Impl* impl, std::shared_ptr<std::atomic<bool>> alive, std::uint64_t requestId,
      std::uint64_t generation
  )
      : m_impl(impl), m_alive(std::move(alive)), m_requestId(requestId), m_generation(generation) {}

  void OnComplete(bool hasDamage) override;

private:
  CefBrowserSession::Impl* m_impl;
  std::shared_ptr<std::atomic<bool>> m_alive;
  std::uint64_t m_requestId;
  std::uint64_t m_generation;

  IMPLEMENT_REFCOUNTING(NoctaliaExternalBeginFrameCallback);
};

// OnScheduleMessagePumpWork may run on any CEF thread. Keep its callback in a
// separately owned synchronization boundary so CEF never dereferences Impl
// while the service is shutting down.
class CefScheduleWorkBridge {
public:
  void set(std::function<void(std::int64_t)> callback) {
    const std::scoped_lock lock(m_mutex);
    m_callback = std::move(callback);
  }

  void dispatch(std::int64_t delayMs) {
    const std::scoped_lock lock(m_mutex);
    if (m_callback) {
      m_callback(delayMs);
    }
  }

  void disable() {
    const std::scoped_lock lock(m_mutex);
    m_callback = nullptr;
  }

private:
  std::mutex m_mutex;
  std::function<void(std::int64_t)> m_callback;
};

struct CefService::Runtime {
  std::string cefDir;
  std::string helperPath;
  CefRefPtr<NoctaliaCefApp> app;
  bool initialized = false;
  GraphicsDevice* graphics = nullptr;
  std::shared_ptr<CefScheduleWorkBridge> scheduleWork = std::make_shared<CefScheduleWorkBridge>();
  std::vector<std::shared_ptr<CefBrowserSession>> sessions;
};

namespace {
  class NoctaliaCefClient;
}

class NoctaliaDevToolsMethodObserver final : public CefDevToolsMessageObserver {
public:
  using ResultCallback = std::function<void(int, bool)>;

  explicit NoctaliaDevToolsMethodObserver(ResultCallback callback) : m_callback(std::move(callback)) {}

  void OnDevToolsMethodResult(
      CefRefPtr<CefBrowser> /*browser*/, int messageId, bool success, const void* /*result*/,
      size_t /*resultSize*/
  ) override {
    if (m_callback) {
      m_callback(messageId, success);
    }
  }

  void OnDevToolsAgentDetached(CefRefPtr<CefBrowser> /*browser*/) override {
    if (m_callback) {
      // A detached agent drops outstanding method results. Zero completes the
      // one bounded presentation transaction regardless of its assigned ID.
      m_callback(0, false);
    }
  }

private:
  ResultCallback m_callback;
  IMPLEMENT_REFCOUNTING(NoctaliaDevToolsMethodObserver);
};

// ---------------------------------------------------------------------------
// Impl: the actual CEF-owning state.
// ---------------------------------------------------------------------------
struct CefBrowserSession::Impl {
  CefService* owner = nullptr;
  std::string id;
  std::string policyId;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefClient> client;
  CefRefPtr<CefDevToolsMessageObserver> devToolsObserver;
  CefRefPtr<CefRegistration> devToolsRegistration;

  bool attached = false;
  bool backgroundPlaybackActive = false;
  std::uint32_t presentationRefreshNs = 0;
  int configuredFrameRate = kCefWindowlessFrameRate;
  CefExternalFrameScheduler frameScheduler{kFallbackBeginFrameIntervalNs};
  CefRendererRecovery rendererRecovery;
  Timer beginFrameAckWatchdog;
  Timer backgroundFrameHeartbeat;
  Timer presentationResizeCaptureTimeout;
  int presentationResizeCaptureMessageId = 0;
  std::function<void()> presentationResizeCaptureReady;
  std::string pendingUrl;
  int logicalWidth = 1280;
  int logicalHeight = 720;
  float deviceScale = 1.0f;

  std::unique_ptr<CefGpuFrameBridge> gpuBridge;
  std::atomic<bool> loggedCpuPaint = false;
  std::atomic<bool> loggedAcceleratedPaint = false;
  TextureHandle texture;
  bool textureChanged = false;
  CefBrowserSessionState state = CefBrowserSessionState::NotCreated;
  bool hasUsableFrame = false;
  std::string lastError;
  std::uint64_t supersededReadyFrames = 0;
  CefPointerMotionCoalescer pointerMotion;
  std::uint32_t pointerButtonFlags = 0;

  std::function<void()> frameReady;
  std::function<void()> frameOpportunity;
  std::function<void(std::uint32_t)> cursorCb;
  std::function<void(CefBrowserSessionState)> stateCb;
  std::function<void(CefBrowserPermissionRequest)> permissionRequestCb;
  std::function<void(CefBrowserContextMenuRequest)> contextMenuRequestCb;
  std::function<void(CefBrowserPopupRequest)> popupCreatedCb;
  std::function<void()> closedCb;
  CefBrowserMediaState mediaState;
  std::function<void(const CefBrowserMediaState&)> mediaStateCb;
  int lastCursorType = -1;
  std::uint32_t lastCursorShape = 0;
  Impl* popupParent = nullptr;
  int popupId = 0;
  int popupPreferredWidth = 0;
  int popupPreferredHeight = 0;
  std::uint64_t nextPopupSerial = 1;
  std::unordered_map<int, std::weak_ptr<CefBrowserSession>> pendingPopups;
  std::weak_ptr<CefBrowserSession> self;
  bool auxiliary = false;
  bool closing = false;
  bool resourcesFinished = false;

  std::shared_ptr<CefBrowserSession> createPendingPopup(
      int requestedPopupId, std::string url, int preferredWidth, int preferredHeight,
      CefRefPtr<CefClient>& outClient
  );
  void abortPendingPopup(int requestedPopupId);

  // Guards deferred main-thread callbacks against use-after-free during shutdown.
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

  void setState(CefBrowserSessionState next, std::string error = {}) {
    const bool changed = state != next || lastError != error;
    state = next;
    lastError = std::move(error);
    if (changed && stateCb) {
      stateCb(state);
    }
  }

  [[nodiscard]] bool frameProductionEnabled() const noexcept { return attached || backgroundPlaybackActive; }

  [[nodiscard]] bool parkedPlaybackRefreshEnabled() const noexcept { return !attached && backgroundPlaybackActive; }

  void completePresentationResizeCapture(int messageId, bool success) {
    if (presentationResizeCaptureMessageId == 0
        || (messageId != 0 && messageId != presentationResizeCaptureMessageId)) {
      return;
    }
    presentationResizeCaptureTimeout.stop();
    presentationResizeCaptureMessageId = 0;
    auto ready = std::move(presentationResizeCaptureReady);
    presentationResizeCaptureReady = nullptr;
    if (!success) {
      kLog.warn("Apple Music viewport-state capture was unavailable; continuing the presentation resize");
    }
    if (!ready) {
      return;
    }
    auto token = alive;
    DeferredCall::callLater([token, ready = std::move(ready)]() mutable {
      if (token->load() && ready) {
        ready();
      }
    });
  }

  void startBackgroundFrameHeartbeat() {
    if (!parkedPlaybackRefreshEnabled() || browser == nullptr) {
      backgroundFrameHeartbeat.stop();
      return;
    }
    if (backgroundFrameHeartbeat.active()) {
      return;
    }
    auto token = alive;
    backgroundFrameHeartbeat.startRepeating(kBackgroundBeginFrameInterval, [this, token]() {
      if (!token->load() || !parkedPlaybackRefreshEnabled() || browser == nullptr) {
        return;
      }
      requestExternalBeginFrame(false);
    });
  }

  void armFrameOpportunity() {
    if (attached && browser != nullptr && frameScheduler.needsFrameOpportunity() && frameOpportunity) {
      frameOpportunity();
    }
  }

  void armPointerMotionOpportunity() {
    // Pointer motion is normally drained by the compositor-paced frame
    // callback. New input also wakes that callback immediately so the callback
    // forwards the newest coalesced position before requesting the urgent
    // external begin frame.
    if (attached && browser != nullptr && frameOpportunity) {
      frameOpportunity();
    }
  }

  void submitExternalBeginFrame(const CefExternalFrameScheduler::Request& request) {
    if (!frameProductionEnabled() || browser == nullptr || request.generation != frameScheduler.generation()) {
      frameScheduler.abandon(request.id);
      return;
    }

    const bool parkedRefresh = parkedPlaybackRefreshEnabled();
    CefExternalBeginFrameArgs args;
    args.deadline_delta_ns = parkedRefresh ? kBackgroundBeginFrameIntervalNs : request.deadlineDeltaNs;
    args.interval_ns = parkedRefresh ? kBackgroundBeginFrameIntervalNs : request.intervalNs;
    CefRefPtr<CefExternalBeginFrameCallback> callback =
        new NoctaliaExternalBeginFrameCallback(this, alive, request.id, request.generation);
    const bool accepted = browser->GetHost()->SendExternalBeginFrameWithTiming(args, callback);
    if (!accepted) {
      frameScheduler.abandon(request.id);
      kLog.warn("CEF rejected timed external begin frame {} (generation {})", request.id, request.generation);
      NOCTALIA_TRACE_PLOT("CEF begin frame outstanding", static_cast<std::int64_t>(0));
      return;
    }
    // The CEF contract is asynchronous, but keep the client correct if an
    // implementation can complete inline and re-enter with the next request.
    if (!frameScheduler.isInFlight(request.id)) {
      return;
    }

    tracy_latency::externalBeginFrameIssued(request.urgent);
    NOCTALIA_TRACE_PLOT("CEF external begin frames", static_cast<std::int64_t>(1));
    NOCTALIA_TRACE_PLOT("CEF begin frame outstanding", static_cast<std::int64_t>(1));
    auto token = alive;
    beginFrameAckWatchdog.stop();
    beginFrameAckWatchdog.start(
        std::chrono::milliseconds(kBeginFrameAckRecoveryNs / 1'000'000), [this, token, request]() {
          if (!token->load()
              || request.generation != frameScheduler.generation()
              || !frameScheduler.isInFlight(request.id)) {
            return;
          }
          kLog.error(
              "CEF external begin frame {} was not acknowledged within the recovery deadline; "
              "aborting the pending Chromium BeginFrame in place",
              request.id
          );
          if (frameProductionEnabled() && browser != nullptr) {
            if (!browser->GetHost()->AbortPendingExternalBeginFrame()) {
              kLog.error("CEF rejected pending external BeginFrame abort");
            }
          }
        }
    );
  }

  void onExternalBeginFrameAck(std::uint64_t requestId, std::uint64_t generation, bool hasDamage) {
    if (generation != frameScheduler.generation() || !frameScheduler.isInFlight(requestId)) {
      return;
    }
    const bool wasDraining = frameScheduler.state() == CefExternalFrameScheduler::State::Draining;
    beginFrameAckWatchdog.stop();
    auto next = frameScheduler.acknowledge(requestId, hasDamage, steadyNowNs());
    NOCTALIA_TRACE_PLOT("CEF begin frame outstanding", static_cast<std::int64_t>(next.has_value()));
    if (wasDraining) {
      if (browser != nullptr) {
        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (frameProductionEnabled()) {
          host->WasHidden(false);
          host->Invalidate(PET_VIEW);
          host->SetFocus(attached);
          requestExternalBeginFrame(true);
        } else {
          host->WasHidden(true);
        }
      }
      return;
    }
    if (next) {
      submitExternalBeginFrame(*next);
    }
    armFrameOpportunity();
  }

  void requestExternalBeginFrame(bool urgent) {
    if (!frameProductionEnabled() || browser == nullptr) {
      return;
    }
    const auto priorState = frameScheduler.state();
    auto request = urgent ? frameScheduler.requestUrgent(steadyNowNs()) : frameScheduler.requestNormal(steadyNowNs());
    if (request) {
      submitExternalBeginFrame(*request);
    } else if (priorState == CefExternalFrameScheduler::State::InFlight) {
      tracy_latency::externalBeginFrameCoalesced();
    }
    armFrameOpportunity();
  }

  void onPresentation(const SurfacePresentationFeedback& feedback) {
    if (!attached) {
      return;
    }
    frameScheduler.onPresentation(feedback);
    if (!feedback.presented) {
      return;
    }
    if (feedback.refreshNs > 0) {
      presentationRefreshNs = feedback.refreshNs;
      const int frameRate =
          std::max(1, static_cast<int>((1'000'000'000ULL + presentationRefreshNs / 2U) / presentationRefreshNs));
      if (browser != nullptr && frameRate != configuredFrameRate) {
        configuredFrameRate = frameRate;
        browser->GetHost()->SetWindowlessFrameRate(frameRate);
        kLog.info("CEF external begin-frame rate updated from presentation feedback: {} fps", frameRate);
      }
    }
    NOCTALIA_TRACE_PLOT("CEF surface refresh ns", static_cast<std::int64_t>(presentationRefreshNs));
  }

  void startExternalScheduler() {
    if (!frameProductionEnabled() || browser == nullptr) {
      return;
    }
    if (frameScheduler.state() == CefExternalFrameScheduler::State::Suspended) {
      frameScheduler.resume();
    }
    requestExternalBeginFrame(false);
    startBackgroundFrameHeartbeat();
  }

  void stopExternalScheduler() {
    backgroundFrameHeartbeat.stop();
    beginFrameAckWatchdog.stop();
    frameScheduler.forceSuspend();
    pointerMotion.reset();
  }

  bool flushPointerMotion() {
    if (browser == nullptr) {
      pointerMotion.reset();
      return false;
    }
    const auto motion = pointerMotion.take();
    if (!motion) {
      return false;
    }
    CefMouseEvent event;
    event.x = motion->x;
    event.y = motion->y;
    event.modifiers = cefModifiersFromKeyMod(motion->modifiers) | pointerButtonFlags;
    tracy_latency::inputForwardedToCef(tracy_latency::InputKind::PointerMove);
    browser->GetHost()->SendMouseMoveEvent(event, false);
    return true;
  }
};

void NoctaliaExternalBeginFrameCallback::OnComplete(bool hasDamage) {
  if (m_alive->load()) {
    m_impl->onExternalBeginFrameAck(m_requestId, m_generation, hasDamage);
  }
}

// ---------------------------------------------------------------------------
// Client: render + lifespan + load + display handlers, delegating to Impl.
// ---------------------------------------------------------------------------
namespace {

  class NoctaliaCefClient : public CefClient,
                            public CefRenderHandler,
                            public CefLifeSpanHandler,
                            public CefLoadHandler,
                            public CefDisplayHandler,
                            public CefRequestHandler,
                            public CefPermissionHandler,
                            public CefDialogHandler,
                            public CefContextMenuHandler,
                            public CefDownloadHandler {
  public:
    explicit NoctaliaCefClient(CefBrowserSession::Impl* impl) : m_impl(impl) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }

    bool OnBeforeBrowse(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
        bool userGesture, bool /*isRedirect*/
    ) override {
      CEF_REQUIRE_UI_THREAD();
      if (m_impl->policyId != "discord" || frame == nullptr || !frame->IsMain() || request == nullptr) {
        return false;
      }
      const std::string url = request->GetURL().ToString();
      if (isAllowedTopLevelUrlForCefSession(m_impl->policyId, url, m_impl->auxiliary)) {
        return false;
      }
      if (userGesture) {
        (void)launchExternalUrl(m_impl->id, url);
      }
      kLog.warn("blocked untrusted top-level navigation from CEF session '{}': {}", m_impl->id, url);
      return true;
    }

    bool OnOpenURLFromTab(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, const CefString& targetUrl,
        WindowOpenDisposition /*targetDisposition*/, bool userGesture
    ) override {
      CEF_REQUIRE_UI_THREAD();
      const std::string url = targetUrl.ToString();
      if (m_impl->policyId != "discord" || isTrustedUrlForCefSession(m_impl->policyId, url)) {
        return false;
      }
      if (userGesture) {
        (void)launchExternalUrl(m_impl->id, url);
      }
      kLog.warn("blocked untrusted open-URL request from CEF session '{}': {}", m_impl->id, url);
      return true;
    }

    bool OnBeforeDownload(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefDownloadItem> /*downloadItem*/,
        const CefString& suggestedName, CefRefPtr<CefBeforeDownloadCallback> callback
    ) override {
      CEF_REQUIRE_UI_THREAD();
      if (callback == nullptr) {
        return true;
      }
      ::FileDialogOptions options;
      options.mode = ::FileDialogMode::Save;
      options.title = "Save download";
      const std::filesystem::path suggested(suggestedName.ToString());
      options.defaultFilename = suggested.filename().string();
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const std::filesystem::path downloads = std::filesystem::path(home) / "Downloads";
        options.startDirectory = std::filesystem::is_directory(downloads) ? downloads : std::filesystem::path(home);
      }
      (void)::FileDialog::open(
          std::move(options), [callback](std::optional<std::filesystem::path> selected) {
            if (selected.has_value()) {
              callback->Continue(CefString(selected->string()), false);
            }
          }
      );
      return true;
    }

    void OnDownloadUpdated(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefDownloadItem> item,
        CefRefPtr<CefDownloadItemCallback> /*callback*/
    ) override {
      CEF_REQUIRE_UI_THREAD();
      if (item == nullptr) {
        return;
      }
      if (item->IsComplete()) {
        kLog.info("CEF session '{}' completed download: {}", m_impl->id, item->GetFullPath().ToString());
      } else if (item->IsCanceled()) {
        kLog.info("CEF session '{}' download was canceled", m_impl->id);
      }
    }

    bool RunContextMenu(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
        CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model,
        CefRefPtr<CefRunContextMenuCallback> callback
    ) override {
      CEF_REQUIRE_UI_THREAD();
      if (m_impl->contextMenuRequestCb == nullptr || params == nullptr || model == nullptr || callback == nullptr) {
        return false;
      }

      CefBrowserContextMenuRequest request;
      request.x = params->GetXCoord();
      request.y = params->GetYCoord();
      request.entries.reserve(model->GetCount());
      for (std::size_t index = 0; index < model->GetCount(); ++index) {
        if (!model->IsVisibleAt(index)) {
          continue;
        }
        const auto type = model->GetTypeAt(index);
        CefBrowserContextMenuEntry entry{
            .commandId = model->GetCommandIdAt(index),
            .label = model->GetLabelAt(index).ToString(),
            .enabled = model->IsEnabledAt(index),
            .separator = type == MENUITEMTYPE_SEPARATOR,
            .checkmark = type == MENUITEMTYPE_CHECK,
            .radio = type == MENUITEMTYPE_RADIO,
            .checked = model->IsCheckedAt(index),
        };
        // The native reusable popup is intentionally flat today. Keep the
        // parent command visible but disabled rather than silently invoking a
        // submenu command without presenting its children.
        if (type == MENUITEMTYPE_SUBMENU) {
          entry.enabled = false;
        }
        request.entries.push_back(std::move(entry));
      }
      if (request.entries.empty()) {
        callback->Cancel();
        return true;
      }

      auto alive = m_impl->alive;
      request.complete = [alive, callback](std::optional<std::int32_t> commandId) {
        if (!alive->load()) {
          return;
        }
        if (commandId.has_value()) {
          callback->Continue(*commandId, EVENTFLAG_NONE);
        } else {
          callback->Cancel();
        }
      };
      m_impl->contextMenuRequestCb(std::move(request));
      return true;
    }

    bool OnFileDialog(
        CefRefPtr<CefBrowser> /*browser*/, FileDialogMode mode, const CefString& title,
        const CefString& defaultFilePath, const std::vector<CefString>& acceptFilters,
        const std::vector<CefString>& /*acceptExtensions*/, const std::vector<CefString>& /*acceptDescriptions*/,
        CefRefPtr<CefFileDialogCallback> callback
    ) override {
      CEF_REQUIRE_UI_THREAD();
      const auto type = static_cast<cef_file_dialog_mode_t>(mode);
      ::FileDialogOptions options;
      if (type == FILE_DIALOG_SAVE) {
        options.mode = ::FileDialogMode::Save;
      } else if (type == FILE_DIALOG_OPEN_FOLDER) {
        options.mode = ::FileDialogMode::SelectFolder;
      } else {
        options.mode = ::FileDialogMode::Open;
      }
      options.title = title.ToString();
      const std::filesystem::path suggested(defaultFilePath.ToString());
      if (!suggested.empty()) {
        options.startDirectory = suggested.has_parent_path() ? suggested.parent_path() : std::filesystem::path{};
        if (type == FILE_DIALOG_SAVE) {
          options.defaultFilename = suggested.filename().string();
        }
      }
      for (const auto& filter : acceptFilters) {
        const std::string value = filter.ToString();
        if (value.starts_with('.') && value.find_first_of("*;/") == std::string::npos) {
          options.extensions.push_back(value);
        }
      }
      if (type == FILE_DIALOG_OPEN_MULTIPLE) {
        kLog.warn("CEF requested multi-file selection; the native picker currently returns one file");
      }
      const bool opened = ::FileDialog::open(
          std::move(options), [callback](std::optional<std::filesystem::path> selected) {
            if (!selected.has_value()) {
              callback->Cancel();
              return;
            }
            callback->Continue({CefString(selected->string())});
          }
      );
      if (!opened) {
        callback->Cancel();
      }
      return true;
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, CefProcessId sourceProcess,
        CefRefPtr<CefProcessMessage> message
    ) override {
      CEF_REQUIRE_UI_THREAD();
      if (sourceProcess != PID_RENDERER || frame == nullptr || !frame->IsMain()
          || message == nullptr || message->GetName() != "noctalia.apple-music.media-state"
          || m_impl->id != "apple-music") {
        return false;
      }
      CefRefPtr<CefListValue> args = message->GetArgumentList();
      if (args == nullptr || args->GetSize() < 5) {
        return true;
      }
      CefBrowserMediaState next{
          .available = !args->GetString(1).empty() || !args->GetString(2).empty(),
          .playing = args->GetBool(0),
          .title = args->GetString(1).ToString(),
          .artist = args->GetString(2).ToString(),
          .album = args->GetString(3).ToString(),
          .artworkUrl = args->GetString(4).ToString(),
      };
      const bool changed = next.available != m_impl->mediaState.available
          || next.playing != m_impl->mediaState.playing || next.title != m_impl->mediaState.title
          || next.artist != m_impl->mediaState.artist || next.album != m_impl->mediaState.album
          || next.artworkUrl != m_impl->mediaState.artworkUrl;
      if (!changed) {
        return true;
      }
      m_impl->mediaState = std::move(next);
      if (m_impl->mediaStateCb) {
        m_impl->mediaStateCb(m_impl->mediaState);
      }
      return true;
    }

    bool OnRequestMediaAccessPermission(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, const CefString& requestingOrigin,
        std::uint32_t requestedPermissions, CefRefPtr<CefMediaAccessCallback> callback
    ) override {
      CEF_REQUIRE_UI_THREAD();
      const std::string origin = requestingOrigin.ToString();
      constexpr std::uint32_t kSupported =
          CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
      if (m_impl->policyId != "discord" || siteIntegrationForUrl(origin) != SiteIntegration::Discord
          || requestedPermissions == 0 || (requestedPermissions & ~kSupported) != 0
          || m_impl->permissionRequestCb == nullptr) {
        kLog.warn(
            "denied media permission for session '{}' origin={} mask=0x{:x}", m_impl->id, origin,
            requestedPermissions
        );
        callback->Cancel();
        return true;
      }

      std::uint32_t publicPermissions = CefBrowserPermissionNone;
      if ((requestedPermissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE) != 0) {
        publicPermissions |= CefBrowserPermissionMicrophone;
      }
      if ((requestedPermissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE) != 0) {
        publicPermissions |= CefBrowserPermissionCamera;
      }
      auto settled = std::make_shared<std::atomic<bool>>(false);
      m_impl->permissionRequestCb(CefBrowserPermissionRequest{
          .origin = origin,
          .permissions = publicPermissions,
          .resolve = [callback, requestedPermissions, settled](std::uint32_t allowedPermissions) {
            if (settled->exchange(true)) {
              return;
            }
            std::uint32_t allowedCefPermissions = 0;
            if ((allowedPermissions & CefBrowserPermissionMicrophone) != 0) {
              allowedCefPermissions |= CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE;
            }
            if ((allowedPermissions & CefBrowserPermissionCamera) != 0) {
              allowedCefPermissions |= CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
            }
            allowedCefPermissions &= requestedPermissions;
            if (allowedCefPermissions != 0) {
              callback->Continue(allowedCefPermissions);
            } else {
              callback->Cancel();
            }
          },
      });
      return true;
    }

    bool OnShowPermissionPrompt(
        CefRefPtr<CefBrowser> /*browser*/, std::uint64_t /*promptId*/, const CefString& requestingOrigin,
        std::uint32_t requestedPermissions, CefRefPtr<CefPermissionPromptCallback> callback
    ) override {
      CEF_REQUIRE_UI_THREAD();
      const std::string origin = requestingOrigin.ToString();
      constexpr std::uint32_t kSupported = CEF_PERMISSION_TYPE_NOTIFICATIONS | CEF_PERMISSION_TYPE_CLIPBOARD;
      if (m_impl->policyId != "discord" || siteIntegrationForUrl(origin) != SiteIntegration::Discord
          || requestedPermissions == 0 || (requestedPermissions & ~kSupported) != 0
          || m_impl->permissionRequestCb == nullptr) {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        return true;
      }
      std::uint32_t publicPermissions = CefBrowserPermissionNone;
      if ((requestedPermissions & CEF_PERMISSION_TYPE_NOTIFICATIONS) != 0) {
        publicPermissions |= CefBrowserPermissionNotifications;
      }
      if ((requestedPermissions & CEF_PERMISSION_TYPE_CLIPBOARD) != 0) {
        publicPermissions |= CefBrowserPermissionClipboard;
      }
      auto settled = std::make_shared<std::atomic<bool>>(false);
      m_impl->permissionRequestCb(CefBrowserPermissionRequest{
          .origin = origin,
          .permissions = publicPermissions,
          .resolve = [callback, publicPermissions, settled](std::uint32_t allowedPermissions) {
            if (!settled->exchange(true)) {
              const bool allAllowed = (allowedPermissions & publicPermissions) == publicPermissions;
              callback->Continue(allAllowed ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
            }
          },
      });
      return true;
    }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> /*browser*/, CefRect& rect) override {
      rect = CefRect(0, 0, m_impl->logicalWidth, m_impl->logicalHeight);
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser> /*browser*/, CefScreenInfo& info) override {
      info.device_scale_factor = m_impl->deviceScale;
      info.rect = CefRect(0, 0, m_impl->logicalWidth, m_impl->logicalHeight);
      info.available_rect = info.rect;
      return true;
    }

    void OnPaint(
        CefRefPtr<CefBrowser> /*browser*/, PaintElementType type, const RectList& /*dirtyRects*/, const void* buffer,
        int width, int height
    ) override {
      (void)type;
      (void)buffer;
      (void)width;
      (void)height;
      if (!m_impl->loggedCpuPaint.exchange(true)) {
        kLog.error(
            "CEF session '{}' delivered a CPU OSR frame despite shared textures being required ({}x{})",
            m_impl->id, width, height
        );
      }
    }

    // CEF's fds are borrowed for this callback only. The bridge duplicates a
    // DMA-BUF only on an import-cache miss. The normal path samples that cached
    // import directly and returns a release fence after Graphite submission;
    // sampling submission returns a token-correlated release fence to CEF.
    void OnAcceleratedPaint(
        CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& /*dirtyRects*/,
        const CefAcceleratedPaintInfo& info
    ) override {
      NOCTALIA_TRACE_ZONE("CEF OnAcceleratedPaint");
      CEF_REQUIRE_UI_THREAD();
      bool adopted = false;
      try {
        if (type != PET_VIEW) {
          return;
        }
        if (m_impl->gpuBridge == nullptr) {
          kLog.error("CEF session '{}' received a frame before its Vulkan bridge was attached", m_impl->id);
          return;
        }
        tracy_latency::acceleratedPaintArrived();
        BorrowedDmabufFrame frame;
        frame.transportEpoch = info.extra.transport_epoch;
        frame.width = info.extra.coded_size.width > 0
            ? info.extra.coded_size.width
            : static_cast<int>(static_cast<float>(m_impl->logicalWidth) * m_impl->deviceScale);
        frame.height = info.extra.coded_size.height > 0
            ? info.extra.coded_size.height
            : static_cast<int>(static_cast<float>(m_impl->logicalHeight) * m_impl->deviceScale);
        if (!info.extra.has_capture_counter) {
          kLog.error("CEF accelerated frame did not include a capture counter");
          return;
        }
        frame.captureCounter = info.extra.capture_counter;
        frame.outputGeneration = info.extra.output_generation;
        frame.outputSlot = info.extra.output_slot;
        frame.contentSerial = info.extra.content_serial;
        frame.fourcc = drmFourccFromCef(info.format);
        frame.modifier = info.modifier;
        frame.queueFamilyIndex = info.extra.queue_family_index;
        frame.producerOldLayout = info.extra.producer_old_layout;
        frame.producerNewLayout = info.extra.producer_new_layout;
        frame.acquireFenceFd = info.acquire_fence_fd;
        frame.planeCount = std::min(info.plane_count, 4);
        if (!m_impl->loggedAcceleratedPaint.exchange(true)) {
          kLog.info(
              "CEF session '{}' received first accelerated frame: {}x{}, fourcc=0x{:08x}, "
              "modifier=0x{:016x}, planes={}",
              m_impl->id, frame.width, frame.height, frame.fourcc, frame.modifier, frame.planeCount
          );
        }
        for (int i = 0; i < frame.planeCount; ++i) {
          frame.planes[i].fd = info.planes[i].fd;
          frame.planes[i].stride = info.planes[i].stride;
          frame.planes[i].offset = info.planes[i].offset;
        }
        if (!m_impl->gpuBridge->acceptFrame(frame)) {
          tracy_latency::acceleratedPaintFailed();
          kLog.error("failed to accept CEF DMA-BUF through Vulkan: {}", m_impl->gpuBridge->lastError());
          if (browser != nullptr
              && frame.transportEpoch != 0
              && frame.captureCounter >= 0
              && frame.acquireFenceFd >= 0) {
            browser->GetHost()->SetAcceleratedPaintReleaseFence(
                frame.transportEpoch, frame.captureCounter, frame.acquireFenceFd
            );
          }
          m_impl->requestExternalBeginFrame(true);
          return;
        }
        adopted = true;
        NOCTALIA_TRACE_FRAME("CEF accelerated frame accepted");
        NOCTALIA_TRACE_PLOT("CEF frame width", static_cast<std::int64_t>(frame.width));
        NOCTALIA_TRACE_PLOT("CEF frame height", static_cast<std::int64_t>(frame.height));
        if (m_impl->textureChanged) {
          ++m_impl->supersededReadyFrames;
          NOCTALIA_TRACE_PLOT(
              "CEF ready frames superseded before adoption", static_cast<std::int64_t>(m_impl->supersededReadyFrames)
          );
        }
        m_impl->texture = m_impl->gpuBridge->texture();
        m_impl->textureChanged = true;
        m_impl->hasUsableFrame = true;
        m_impl->setState(CefBrowserSessionState::Ready);
        if (m_impl->frameReady) {
          m_impl->frameReady();
        }
      } catch (const std::exception& exception) {
        kLog.error("CEF accelerated-paint callback failed: {}", exception.what());
        if (!adopted
            && browser != nullptr
            && info.extra.transport_epoch != 0
            && info.extra.has_capture_counter
            && info.acquire_fence_fd >= 0) {
          browser->GetHost()->SetAcceleratedPaintReleaseFence(
              info.extra.transport_epoch, info.extra.capture_counter, info.acquire_fence_fd
          );
        }
      } catch (...) {
        kLog.error("CEF accelerated-paint callback failed with an unknown exception");
        if (!adopted
            && browser != nullptr
            && info.extra.transport_epoch != 0
            && info.extra.has_capture_counter
            && info.acquire_fence_fd >= 0) {
          browser->GetHost()->SetAcceleratedPaintReleaseFence(
              info.extra.transport_epoch, info.extra.capture_counter, info.acquire_fence_fd
          );
        }
      }
    }

    bool OnCursorChange(
        CefRefPtr<CefBrowser> /*browser*/, CefCursorHandle /*cursor*/, cef_cursor_type_t type,
        const CefCursorInfo& /*info*/
    ) override {
      const std::uint32_t shape = cursorShapeFromCef(type);
      if (m_impl->lastCursorType != static_cast<int>(type) || m_impl->lastCursorShape != shape) {
        kLog.debug("CEF cursor changed: type={} shape={}", static_cast<int>(type), shape);
        m_impl->lastCursorType = static_cast<int>(type);
        m_impl->lastCursorShape = shape;
      }
      auto alive = m_impl->alive;
      auto* impl = m_impl;
      DeferredCall::callLater([alive, impl, shape]() {
        if (alive->load() && impl->cursorCb) {
          impl->cursorCb(shape);
        }
      });
      return true; // we render the cursor via the Wayland cursor-shape protocol
    }

    // CefLifeSpanHandler
    bool OnBeforePopup(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, int popupId, const CefString& targetUrl,
        const CefString& /*targetFrameName*/, WindowOpenDisposition targetDisposition, bool userGesture,
        const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client,
        CefBrowserSettings& settings, CefRefPtr<CefDictionaryValue>& /*extraInfo*/, bool* noJavascriptAccess
    ) override {
      CEF_REQUIRE_UI_THREAD();
      const std::string url = targetUrl.ToString();
      if (!isAllowedTopLevelUrlForCefSession(m_impl->policyId, url, true)) {
        if (userGesture) {
          (void)launchExternalUrl(m_impl->id, url);
        }
        kLog.warn("blocked untrusted popup from CEF session '{}': {}", m_impl->id, url);
        return true;
      }
      if (m_impl->owner == nullptr || m_impl->gpuBridge == nullptr) {
        kLog.error("blocked CEF popup because the mandatory Vulkan session runtime is unavailable");
        return true;
      }
      if (m_impl->popupCreatedCb == nullptr) {
        kLog.warn("blocked CEF popup because session '{}' has no active Noctalia presenter", m_impl->id);
        return true;
      }
      const int preferredWidth = popupFeatures.widthSet ? popupFeatures.width : 0;
      const int preferredHeight = popupFeatures.heightSet ? popupFeatures.height : 0;
      auto popup = m_impl->createPendingPopup(popupId, url, preferredWidth, preferredHeight, client);
      if (popup == nullptr || client == nullptr) {
        kLog.error("failed to allocate accelerated windowless popup session");
        return true;
      }

      windowInfo.SetAsWindowless(0);
      windowInfo.shared_texture_enabled = 1;
      windowInfo.external_begin_frame_enabled = 1;
      settings.windowless_frame_rate = m_impl->configuredFrameRate;
      settings.background_color = CefColorSetARGB(0, 0, 0, 0);
      if (noJavascriptAccess != nullptr) {
        *noJavascriptAccess = false;
      }
      kLog.info(
          "accepted accelerated windowless popup: parent={} child={} popup={} disposition={} size={}x{}",
          m_impl->id, popup->id(), popupId, static_cast<int>(targetDisposition),
          preferredWidth > 0 ? preferredWidth : m_impl->logicalWidth,
          preferredHeight > 0 ? preferredHeight : m_impl->logicalHeight
      );
      return false;
    }

    void OnBeforePopupAborted(CefRefPtr<CefBrowser> /*browser*/, int popupId) override {
      CEF_REQUIRE_UI_THREAD();
      m_impl->abortPendingPopup(popupId);
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
      CEF_REQUIRE_UI_THREAD();
      m_impl->browser = browser;
      m_impl->setState(CefBrowserSessionState::Loading);
      auto alive = m_impl->alive;
      auto* impl = m_impl;
      m_impl->devToolsObserver = new NoctaliaDevToolsMethodObserver(
          [alive, impl](int messageId, bool success) {
            if (alive->load()) {
              impl->completePresentationResizeCapture(messageId, success);
            }
          }
      );
      m_impl->devToolsRegistration =
          browser->GetHost()->AddDevToolsMessageObserver(m_impl->devToolsObserver);
      browser->GetHost()->WasHidden(!m_impl->frameProductionEnabled());
      browser->GetHost()->WasResized();
      if (m_impl->frameProductionEnabled()) {
        browser->GetHost()->Invalidate(PET_VIEW);
        browser->GetHost()->SetFocus(m_impl->attached);
      }
      m_impl->startExternalScheduler();
      kLog.info("CEF browser created for session '{}'", m_impl->id);
      if (m_impl->auxiliary && m_impl->popupParent != nullptr) {
        auto* parent = m_impl->popupParent;
        parent->pendingPopups.erase(m_impl->popupId);
        if (parent->alive->load() && parent->popupCreatedCb) {
          if (auto self = m_impl->self.lock()) {
            parent->popupCreatedCb(CefBrowserPopupRequest{
                .session = std::move(self),
                .preferredWidth = m_impl->popupPreferredWidth,
                .preferredHeight = m_impl->popupPreferredHeight,
            });
          }
        }
      }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override {
      m_impl->stopExternalScheduler();
      // Every close path, including JavaScript window.close(), DevTools, and
      // opener teardown, must return the last exported image before dropping
      // the browser used by the bridge's release callback. The native close
      // affordance already drains proactively, but it cannot be the ownership
      // invariant because web content can close an auxiliary browser itself.
      if (m_impl->gpuBridge != nullptr) {
        m_impl->gpuBridge->discardPendingFrame();
      }
      m_impl->presentationResizeCaptureTimeout.stop();
      m_impl->presentationResizeCaptureMessageId = 0;
      m_impl->presentationResizeCaptureReady = nullptr;
      m_impl->devToolsRegistration = nullptr;
      m_impl->devToolsObserver = nullptr;
      m_impl->rendererRecovery.reset();
      m_impl->browser = nullptr;
      m_impl->closing = false;
      if (m_impl->closedCb) {
        m_impl->closedCb();
      }
    }

    // CefRequestHandler
    void OnRenderViewReady(CefRefPtr<CefBrowser> browser) override {
      CEF_REQUIRE_UI_THREAD();
      const bool recovered = m_impl->rendererRecovery.state() != CefRendererRecovery::State::Ready;
      m_impl->rendererRecovery.onRenderViewReady();
      if (!m_impl->frameProductionEnabled() || browser == nullptr) {
        return;
      }
      browser->GetHost()->WasResized();
      browser->GetHost()->Invalidate(PET_VIEW);
      browser->GetHost()->WasHidden(false);
      browser->GetHost()->SetFocus(m_impl->attached);
      m_impl->startExternalScheduler();
      m_impl->requestExternalBeginFrame(true);
      if (recovered) {
        kLog.info("CEF renderer recovery produced a ready render view");
      }
    }

    void OnRenderProcessTerminated(
        CefRefPtr<CefBrowser> browser, TerminationStatus status, int errorCode, const CefString& errorString
    ) override {
      CEF_REQUIRE_UI_THREAD();
      m_impl->stopExternalScheduler();
      if (m_impl->gpuBridge != nullptr) {
        m_impl->gpuBridge->discardPendingFrame();
      }

      const auto action = m_impl->rendererRecovery.onTerminated();
      m_impl->setState(
          action == CefRendererRecovery::Action::Reload ? CefBrowserSessionState::Recovering
                                                       : CefBrowserSessionState::Fatal,
          errorString.ToString()
      );
      kLog.error(
          "CEF renderer for session '{}' terminated: status={} code={} error={} recovery={}", m_impl->id,
          static_cast<int>(status), errorCode, errorString.ToString(),
          action == CefRendererRecovery::Action::Reload ? "reload" : "stopped"
      );
      if (action != CefRendererRecovery::Action::Reload || browser == nullptr) {
        return;
      }

      auto alive = m_impl->alive;
      auto* impl = m_impl;
      const int browserId = browser->GetIdentifier();
      const std::uint64_t recoveryGeneration = m_impl->rendererRecovery.generation();
      DeferredCall::callLater([alive, impl, browserId, recoveryGeneration]() {
        if (!alive->load()
            || impl->browser == nullptr
            || impl->browser->GetIdentifier() != browserId
            || !impl->rendererRecovery.isPending(recoveryGeneration)) {
          return;
        }
        // Reload uses the existing persistent request context and HTTP cache.
        // The new render view's ready callback restarts external begin frames.
        impl->browser->Reload();
      });
    }

    // CefLoadHandler
    void OnLoadingStateChange(
        CefRefPtr<CefBrowser> /*browser*/, bool isLoading, bool /*canGoBack*/, bool /*canGoForward*/
    ) override {
      kLog.info("CEF session '{}' loading state changed: {}", m_impl->id, isLoading ? "loading" : "idle");
      if (isLoading && !m_impl->hasUsableFrame) {
        m_impl->setState(CefBrowserSessionState::Loading);
      }
      m_impl->requestExternalBeginFrame(isLoading);
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, int httpStatusCode) override {
      if (frame->IsMain()) {
        kLog.info(
            "CEF session '{}' main frame loaded: status={} url={}", m_impl->id, httpStatusCode,
            frame->GetURL().ToString()
        );
      }
    }

    void OnLoadError(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText,
        const CefString& failedUrl
    ) override {
      if (frame->IsMain()) {
        kLog.error(
            "CEF session '{}' main frame load failed: code={} error={} url={}", m_impl->id,
            static_cast<int>(errorCode), errorText.ToString(), failedUrl.ToString()
        );
        m_impl->setState(CefBrowserSessionState::Failed, errorText.ToString());
      }
    }

  private:
    CefBrowserSession::Impl* m_impl;
    IMPLEMENT_REFCOUNTING(NoctaliaCefClient);
  };

} // namespace

std::shared_ptr<CefBrowserSession> CefBrowserSession::Impl::createPendingPopup(
    int requestedPopupId, std::string url, int preferredWidth, int preferredHeight,
    CefRefPtr<CefClient>& outClient
) {
  if (owner == nullptr) {
    return nullptr;
  }
  const std::string childId = std::format("{}-popup-{}-{}", id, requestedPopupId, nextPopupSerial++);
  auto popup = owner->createBrowserSession(childId);
  auto* child = popup->m_impl.get();
  child->auxiliary = true;
  child->policyId = policyId;
  child->popupParent = this;
  child->popupId = requestedPopupId;
  child->popupPreferredWidth = preferredWidth;
  child->popupPreferredHeight = preferredHeight;
  child->logicalWidth = preferredWidth > 0 ? preferredWidth : logicalWidth;
  child->logicalHeight = preferredHeight > 0 ? preferredHeight : logicalHeight;
  child->pendingUrl = std::move(url);
  child->setState(CefBrowserSessionState::Loading);
  child->client = new NoctaliaCefClient(child);
  outClient = child->client;
  pendingPopups.insert_or_assign(requestedPopupId, popup);
  return popup;
}

void CefBrowserSession::Impl::abortPendingPopup(int requestedPopupId) {
  const auto found = pendingPopups.find(requestedPopupId);
  if (found == pendingPopups.end()) {
    return;
  }
  if (auto popup = found->second.lock()) {
    popup->m_impl->client = nullptr;
    popup->m_impl->alive->store(false);
    popup->m_impl->setState(CefBrowserSessionState::Failed, "Popup creation was aborted");
  }
  pendingPopups.erase(found);
}

// ---------------------------------------------------------------------------
// CefService
// ---------------------------------------------------------------------------
CefService::CefService(std::string cefDir, std::string helperPath) : m_runtime(std::make_unique<Runtime>()) {
  m_runtime->cefDir = std::move(cefDir);
  m_runtime->helperPath = helperPath.empty() ? helperNextToSelf() : std::move(helperPath);
}

CefService::~CefService() { shutdown(); }

bool CefService::initialized() const noexcept { return m_runtime->initialized; }

std::shared_ptr<CefBrowserSession> CefService::createBrowserSession(std::string id) {
  for (const auto& session : m_runtime->sessions) {
    if (session != nullptr && session->id() == id) {
      return session;
    }
  }
  auto session = std::shared_ptr<CefBrowserSession>(new CefBrowserSession(*this, std::move(id)));
  session->m_impl->self = session;
  if (m_runtime->graphics != nullptr) {
    session->attachGraphicsDevice(*m_runtime->graphics);
  }
  m_runtime->sessions.push_back(session);
  return session;
}

bool CefService::initialize() {
  if (m_runtime->initialized) {
    return true;
  }

  CefMainArgs mainArgs(0, nullptr);

  auto scheduleWork = m_runtime->scheduleWork;
  m_runtime->app = new NoctaliaCefApp([scheduleWork = std::move(scheduleWork)](std::int64_t delayMs) {
    scheduleWork->dispatch(delayMs);
  });

  const std::string rootCachePath = userCachePath();
  if (const char* widevine = std::getenv("NOCTALIA_CEF_WIDEVINE"); widevine != nullptr && widevine[0] != '\0') {
    std::string widevineError;
    if (!prepareNoctaliaWidevineHint(rootCachePath, widevine, widevineError)) {
      kLog.error("{}", widevineError);
      return false;
    }
  }

  CefSettings settings;
  settings.no_sandbox = true;
  settings.windowless_rendering_enabled = true;
  settings.multi_threaded_message_loop = false;
  settings.external_message_pump = true;
  settings.log_severity = LOGSEVERITY_WARNING;
  if (const char* value = std::getenv("NOCTALIA_CEF_REMOTE_DEBUGGING_PORT");
      value != nullptr && value[0] != '\0') {
    char* end = nullptr;
    const long port = std::strtol(value, &end, 10);
    if (end != value && *end == '\0' && port >= 1024 && port <= 65535) {
      settings.remote_debugging_port = static_cast<int>(port);
      kLog.info("CEF remote debugging enabled on loopback port {}", port);
    } else {
      kLog.warn("ignoring invalid NOCTALIA_CEF_REMOTE_DEBUGGING_PORT={}", value);
    }
  }
  // Apple Music uses session cookies for part of its authentication state.
  // Keep those cookies in the persistent CEF profile across shell restarts.
  settings.persist_session_cookies = true;
  // A root_cache_path alone does not create a persistent request context.
  // Reuse the existing root as the global profile so enabling persistence does
  // not relocate or discard the current Apple Music browser data.
  CefString(&settings.root_cache_path).FromString(rootCachePath);
  CefString(&settings.cache_path).FromString(rootCachePath);
  CefString(&settings.resources_dir_path).FromString(m_runtime->cefDir + "/Resources");
  CefString(&settings.locales_dir_path).FromString(m_runtime->cefDir + "/Resources/locales");
  if (!m_runtime->helperPath.empty()) {
    CefString(&settings.browser_subprocess_path).FromString(m_runtime->helperPath);
  }

  if (!CefInitialize(mainArgs, settings, m_runtime->app.get(), nullptr)) {
    m_runtime->app = nullptr;
    return false;
  }
  m_runtime->initialized = true;
  kLog.info("CEF persistent profile enabled at {}", rootCachePath);
  return true;
}

void CefService::attachGraphicsDevice(GraphicsDevice& graphics) {
  if (!graphics.valid() || !graphics.cefExternalMemoryEnabled()) {
    throw std::runtime_error("CEF requires the Vulkan external-memory GraphicsDevice contract");
  }
  m_runtime->graphics = &graphics;
  for (const auto& session : m_runtime->sessions) {
    session->attachGraphicsDevice(graphics);
  }
}

void CefService::prepareForGraphicsDeviceRebuild() {
  m_runtime->graphics = nullptr;
  for (const auto& session : m_runtime->sessions) {
    session->prepareForGraphicsDeviceRebuild();
  }
}

void CefService::resumeAfterGraphicsDeviceRebuild(GraphicsDevice& graphics) {
  if (!graphics.valid() || !graphics.cefExternalMemoryEnabled()) {
    throw std::runtime_error("CEF requires the Vulkan external-memory GraphicsDevice contract");
  }
  m_runtime->graphics = &graphics;
  for (const auto& session : m_runtime->sessions) {
    session->resumeAfterGraphicsDeviceRebuild(graphics);
  }
}

void CefService::shutdown() {
  if (!m_runtime->initialized) {
    return;
  }
  m_runtime->scheduleWork->disable();
  for (const auto& session : m_runtime->sessions) {
    session->beginShutdown();
  }
  for (int i = 0; i < 500; ++i) {
    bool allClosed = true;
    for (const auto& session : m_runtime->sessions) {
      allClosed = allClosed && session->browserClosed();
    }
    if (allClosed) {
      break;
    }
    CefDoMessageLoopWork();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  for (const auto& session : m_runtime->sessions) {
    if (!session->browserClosed()) {
      kLog.error("CEF session '{}' did not reach OnBeforeClose before shutdown", session->id());
    }
    session->finishShutdown();
  }
  CefShutdown();
  m_runtime->app = nullptr;
  m_runtime->initialized = false;
  m_runtime->graphics = nullptr;
}

// ---------------------------------------------------------------------------
// CefBrowserSession
// ---------------------------------------------------------------------------
CefBrowserSession::CefBrowserSession(CefService& service, std::string id)
    : m_impl(std::make_unique<Impl>()), m_id(std::move(id)) {
  m_impl->owner = &service;
  m_impl->id = m_id;
  m_impl->policyId = m_id;
}

CefBrowserSession::~CefBrowserSession() { finishShutdown(); }

void CefBrowserSession::attachGraphicsDevice(GraphicsDevice& graphics) {
  if (m_impl->gpuBridge != nullptr) {
    return;
  }
  auto alive = m_impl->alive;
  auto* impl = m_impl.get();
  m_impl->gpuBridge = std::make_unique<CefGpuFrameBridge>(
      graphics, graphics.textureManager(),
      [alive, impl](std::uint64_t transportEpoch, std::int64_t captureCounter, int fd) {
        if (!alive->load() || impl->browser == nullptr) {
          kLog.warn("dropping CEF release fence for closed session '{}' frame {}", impl->id, captureCounter);
          return;
        }
        impl->browser->GetHost()->SetAcceleratedPaintReleaseFence(transportEpoch, captureCounter, fd);
      }
  );
}

void CefBrowserSession::prepareForGraphicsDeviceRebuild() {
  m_impl->stopExternalScheduler();
  if (m_impl->gpuBridge != nullptr) {
    m_impl->gpuBridge->abandonDevice();
    m_impl->gpuBridge.reset();
  }
  m_impl->texture = {};
  m_impl->textureChanged = true;
  m_impl->hasUsableFrame = false;
  m_impl->setState(CefBrowserSessionState::Recovering);
}

void CefBrowserSession::resumeAfterGraphicsDeviceRebuild(GraphicsDevice& graphics) {
  attachGraphicsDevice(graphics);
  m_impl->texture = {};
  m_impl->textureChanged = true;
  if (m_impl->browser != nullptr) {
    m_impl->browser->GetHost()->WasHidden(!m_impl->frameProductionEnabled());
    m_impl->browser->GetHost()->Invalidate(PET_VIEW);
    m_impl->browser->GetHost()->WasResized();
    m_impl->startExternalScheduler();
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::beginShutdown() {
  m_impl->stopExternalScheduler();
  // Return ownership of the last sampled export while the browser still
  // exists to receive its release fence. Destroying the browser first leaves
  // Viz's native-pixmap access open and makes the GPU process treat orderly
  // shell shutdown as an unrecoverable ownership error.
  if (m_impl->gpuBridge != nullptr) {
    m_impl->gpuBridge->discardPendingFrame();
  }
  m_impl->alive->store(false);
  if (m_impl->browser != nullptr) {
    m_impl->browser->GetHost()->CloseBrowser(true);
  }
}

bool CefBrowserSession::browserClosed() const noexcept { return m_impl->browser == nullptr; }

void CefBrowserSession::finishShutdown() {
  if (m_impl->resourcesFinished) {
    return;
  }
  m_impl->resourcesFinished = true;
  m_impl->client = nullptr;
  if (m_impl->gpuBridge != nullptr) {
    const CefGpuFrameBridgeStats stats = m_impl->gpuBridge->stats();
    kLog.info(
        "CEF session '{}' GPU bridge summary: accepted={}, directStaged={}, "
        "directSampled={}, directDiscarded={}, releaseFences={}, imports={}/{}, "
        "activeImports={}, cacheHits={}, cacheMisses={}, cacheEvictions={}",
        m_id, stats.framesAccepted, stats.directFramesStaged, stats.directFramesSampled,
        stats.directFramesDiscarded, stats.releaseFenceFdsExported, stats.importsCreated,
        stats.importsDestroyed, stats.activeImports, stats.importCacheHits, stats.importCacheMisses,
        stats.importCacheEvictions
    );
  }
  m_impl->gpuBridge.reset();
}

void CefBrowserSession::ensureBrowser(int logicalWidth, int logicalHeight) {
  // Once the browser exists, resize() owns viewport changes and must compare
  // the requested dimensions with the dimensions CEF currently knows. Do not
  // overwrite that state here: CefSurfaceNode deliberately calls
  // ensureBrowser() followed by resize(), and doing so would make resize()
  // incorrectly suppress WasResized().
  if (m_impl->browser) {
    return;
  }
  m_impl->logicalWidth = logicalWidth > 0 ? logicalWidth : m_impl->logicalWidth;
  m_impl->logicalHeight = logicalHeight > 0 ? logicalHeight : m_impl->logicalHeight;
  if (m_impl->gpuBridge == nullptr) {
    m_impl->setState(CefBrowserSessionState::Fatal, "Vulkan DMA-BUF bridge unavailable");
    kLog.error("refusing to create CEF browser: mandatory Vulkan DMA-BUF bridge is unavailable");
    return;
  }
  if (m_impl->owner == nullptr || (!m_impl->owner->initialized() && !m_impl->owner->initialize())) {
    m_impl->setState(CefBrowserSessionState::Fatal, "CEF initialization failed");
    return;
  }
  m_impl->setState(CefBrowserSessionState::Loading);
  m_impl->client = new NoctaliaCefClient(m_impl.get());

  CefWindowInfo windowInfo;
  windowInfo.SetAsWindowless(0);
  windowInfo.shared_texture_enabled = 1;
  windowInfo.external_begin_frame_enabled = 1;
  CefBrowserSettings browserSettings;
  browserSettings.windowless_frame_rate = kCefWindowlessFrameRate;
  browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

  const std::string url = m_impl->pendingUrl.empty() ? std::string("about:blank") : m_impl->pendingUrl;
  if (!CefBrowserHost::CreateBrowser(windowInfo, m_impl->client, url, browserSettings, nullptr, nullptr)) {
    kLog.error("CEF browser creation request was rejected");
  } else {
    kLog.info(
        "CEF browser creation requested for session '{}': {}x{} fps={} scheduler={} url={}", m_id,
        m_impl->logicalWidth, m_impl->logicalHeight, kCefWindowlessFrameRate, "wayland-frame-callback", url
    );
  }
}

void CefBrowserSession::resize(int logicalWidth, int logicalHeight) {
  if (logicalWidth <= 0 || logicalHeight <= 0) {
    return;
  }
  if (logicalWidth == m_impl->logicalWidth && logicalHeight == m_impl->logicalHeight) {
    return;
  }
  m_impl->logicalWidth = logicalWidth;
  m_impl->logicalHeight = logicalHeight;
  if (m_impl->browser) {
    m_impl->browser->GetHost()->WasResized();
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::navigate(const std::string& url) {
  m_impl->pendingUrl = url;
  if (!m_impl->hasUsableFrame) {
    m_impl->setState(CefBrowserSessionState::Loading);
  }
  if (m_impl->browser) {
    m_impl->browser->GetMainFrame()->LoadURL(url);
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::reload() {
  m_impl->lastError.clear();
  m_impl->setState(CefBrowserSessionState::Loading);
  if (m_impl->browser != nullptr) {
    m_impl->browser->Reload();
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::close() {
  if (m_impl->closing) {
    return;
  }
  m_impl->closing = true;
  m_impl->stopExternalScheduler();
  if (m_impl->gpuBridge != nullptr) {
    m_impl->gpuBridge->discardPendingFrame();
  }
  if (m_impl->browser != nullptr) {
    m_impl->browser->GetHost()->CloseBrowser(true);
    return;
  }
  m_impl->closing = false;
  if (m_impl->closedCb) {
    m_impl->closedCb();
  }
}

void CefBrowserSession::execJs(const std::string& code) {
  if (m_impl->browser) {
    m_impl->browser->GetMainFrame()->ExecuteJavaScript(code, m_impl->browser->GetMainFrame()->GetURL(), 0);
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::preparePresentationResize(std::function<void()> ready) {
  if (!ready) {
    return;
  }
  if (m_impl->browser == nullptr || m_impl->devToolsRegistration == nullptr) {
    ready();
    return;
  }

  // There is only one fullscreen transition owner. Still terminate an older
  // request defensively so renderer detachment or rapid input cannot strand
  // either state machine.
  if (m_impl->presentationResizeCaptureMessageId != 0) {
    m_impl->completePresentationResizeCapture(0, false);
  }

  CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
  params->SetString(
      "expression",
      "Boolean(globalThis.__noctaliaAppleMusicLyricsResizeV1?.capture?.())"
  );
  params->SetBool("returnByValue", true);
  const int messageId =
      m_impl->browser->GetHost()->ExecuteDevToolsMethod(0, "Runtime.evaluate", params);
  if (messageId == 0) {
    ready();
    return;
  }

  m_impl->presentationResizeCaptureMessageId = messageId;
  m_impl->presentationResizeCaptureReady = std::move(ready);
  auto alive = m_impl->alive;
  auto* impl = m_impl.get();
  m_impl->presentationResizeCaptureTimeout.start(kPresentationResizeCaptureTimeout, [alive, impl, messageId]() {
    if (!alive->load() || impl->presentationResizeCaptureMessageId != messageId) {
      return;
    }
    impl->completePresentationResizeCapture(messageId, false);
  });
}

void CefBrowserSession::setDeviceScale(float scale) {
  if (scale <= 0.0f || scale == m_impl->deviceScale) {
    return;
  }
  m_impl->deviceScale = scale;
  if (m_impl->browser) {
    m_impl->browser->GetHost()->WasResized();
    m_impl->browser->GetHost()->NotifyScreenInfoChanged();
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::sendMouseMove(float x, float y, std::uint32_t modifiers, bool leaving) {
  if (!m_impl->browser) {
    return;
  }
  if (!leaving) {
    if (m_impl->pointerMotion.queue({static_cast<int>(x), static_cast<int>(y), modifiers})) {
      m_impl->armPointerMotionOpportunity();
    }
    return;
  }
  m_impl->pointerMotion.reset();
  CefMouseEvent event;
  event.x = static_cast<int>(x);
  event.y = static_cast<int>(y);
  event.modifiers = cefModifiersFromKeyMod(modifiers) | m_impl->pointerButtonFlags;
  tracy_latency::inputForwardedToCef(tracy_latency::InputKind::PointerMove);
  m_impl->browser->GetHost()->SendMouseMoveEvent(event, true);
  m_impl->requestExternalBeginFrame(true);
}

void CefBrowserSession::flushMouseMove() {
  if (m_impl->flushPointerMotion()) {
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::sendMouseButton(
    float x, float y, int button, bool pressed, int clickCount, std::uint32_t modifiers
) {
  if (!m_impl->browser) {
    return;
  }
  m_impl->flushPointerMotion();
  const std::uint32_t buttonFlag = cefButtonFlag(button);
  if (pressed) {
    m_impl->pointerButtonFlags |= buttonFlag;
  } else {
    m_impl->pointerButtonFlags &= ~buttonFlag;
  }
  CefMouseEvent event;
  event.x = static_cast<int>(x);
  event.y = static_cast<int>(y);
  event.modifiers = cefModifiersFromKeyMod(modifiers) | m_impl->pointerButtonFlags;
  cef_mouse_button_type_t type = MBT_LEFT;
  if (button == 1) {
    type = MBT_MIDDLE;
  } else if (button == 2) {
    type = MBT_RIGHT;
  }
  if (pressed) {
    tracy_latency::inputForwardedToCef(tracy_latency::InputKind::PointerButton);
  }
  m_impl->browser->GetHost()->SendMouseClickEvent(event, type, !pressed, clickCount);
  m_impl->requestExternalBeginFrame(true);
}

void CefBrowserSession::sendMouseWheel(
    float x, float y, float deltaX, float deltaY, std::uint32_t modifiers
) {
  if (!m_impl->browser) {
    return;
  }
  m_impl->flushPointerMotion();
  CefMouseEvent event;
  event.x = static_cast<int>(x);
  event.y = static_cast<int>(y);
  event.modifiers = cefModifiersFromKeyMod(modifiers) | m_impl->pointerButtonFlags;
  tracy_latency::inputForwardedToCef(tracy_latency::InputKind::PointerWheel);
  m_impl->browser->GetHost()->SendMouseWheelEvent(event, static_cast<int>(deltaX), static_cast<int>(deltaY));
  m_impl->requestExternalBeginFrame(true);
}

void CefBrowserSession::sendKey(
    std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed
) {
  if (!m_impl->browser) {
    return;
  }
  const std::uint32_t flags = cefModifiersFromKeyMod(modifiers);
  const int vk = windowsKeyCodeFromSym(sym);

  CefKeyEvent event;
  event.modifiers = flags;
  event.windows_key_code = vk;
  event.native_key_code = static_cast<int>(sym);
  event.type = pressed ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
  if (pressed) {
    tracy_latency::inputForwardedToCef(tracy_latency::InputKind::Key);
  }
  m_impl->browser->GetHost()->SendKeyEvent(event);

  // Emit a CHAR event for printable input so text lands in fields.
  if (pressed && utf32 != 0) {
    CefKeyEvent charEvent;
    charEvent.modifiers = flags;
    charEvent.windows_key_code = static_cast<int>(utf32);
    charEvent.character = static_cast<char16_t>(utf32);
    charEvent.unmodified_character = static_cast<char16_t>(utf32);
    charEvent.type = KEYEVENT_CHAR;
    m_impl->browser->GetHost()->SendKeyEvent(charEvent);
  }
  m_impl->requestExternalBeginFrame(true);
}

void CefBrowserSession::goBack() {
  if (!m_impl->browser || !m_impl->browser->CanGoBack()) {
    return;
  }
  m_impl->browser->GoBack();
  m_impl->requestExternalBeginFrame(true);
}

void CefBrowserSession::goForward() {
  if (!m_impl->browser || !m_impl->browser->CanGoForward()) {
    return;
  }
  m_impl->browser->GoForward();
  m_impl->requestExternalBeginFrame(true);
}

void CefBrowserSession::setFocus(bool focused) {
  if (m_impl->browser) {
    m_impl->browser->GetHost()->SetFocus(focused);
    if (focused) {
      m_impl->requestExternalBeginFrame(true);
    }
  }
}

void CefBrowserSession::setDisplayAttached(bool attached) {
  if (m_impl->attached == attached) {
    return;
  }
  m_impl->attached = attached;
  CefRefPtr<CefBrowser> browser = m_impl->browser;
  if (!browser) {
    return;
  }
  CefRefPtr<CefBrowserHost> host = browser->GetHost();
  if (!host) {
    kLog.warn("CEF browser host unavailable while changing display attachment to {}", attached);
    return;
  }
  if (attached) {
    m_impl->backgroundFrameHeartbeat.stop();
    if (m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Suspended
        || m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Draining) {
      m_impl->frameScheduler.resume();
    }
    if (m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Draining) {
      return;
    }
    host->WasHidden(false);
    host->Invalidate(PET_VIEW);
    host->SetFocus(true);
    m_impl->requestExternalBeginFrame(true);
  } else if (m_impl->backgroundPlaybackActive) {
    // Keep Chromium logically visible but unfocused so a compositor-less,
    // acknowledged 1 Hz BeginFrame can keep the parked playback UI current.
    // The newest exported DMA-BUF stays leased and becomes the immediate
    // reopen frame; no Noctalia surface is rendered while the panel is closed.
    host->SetFocus(false);
    host->WasHidden(false);
    if (m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Suspended
        || m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Draining) {
      m_impl->frameScheduler.resume();
    }
    m_impl->startBackgroundFrameHeartbeat();
  } else {
    m_impl->backgroundFrameHeartbeat.stop();
    m_impl->beginFrameAckWatchdog.stop();
    const bool abortAlreadyPending = m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Draining;
    const bool abortRequired = m_impl->frameScheduler.suspend();
    m_impl->pointerMotion.reset();
    host->SetFocus(false);
    if (abortRequired) {
      if (!abortAlreadyPending && !host->AbortPendingExternalBeginFrame()) {
        kLog.error("CEF rejected external BeginFrame abort while detaching display");
        m_impl->frameScheduler.forceSuspend();
        host->WasHidden(true);
      }
      return;
    }
    host->WasHidden(true);
  }
}

void CefBrowserSession::setBackgroundPlaybackActive(bool active) {
  const bool changed = m_impl->backgroundPlaybackActive != active;
  m_impl->backgroundPlaybackActive = active;

  if (!changed && !active) {
    return;
  }

  if (m_impl->attached || m_impl->browser == nullptr) {
    m_impl->backgroundFrameHeartbeat.stop();
    return;
  }

  CefRefPtr<CefBrowserHost> host = m_impl->browser->GetHost();
  if (!host) {
    kLog.warn("CEF browser host unavailable while changing background playback state to {}", active);
    return;
  }

  if (active) {
    host->SetFocus(false);
    host->WasHidden(false);
    if (m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Suspended
        || m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Draining) {
      m_impl->frameScheduler.resume();
    }
    m_impl->startBackgroundFrameHeartbeat();
    // MPRIS change callbacks are significant rather than position ticks.
    // Force one immediate frame for a new track/artwork or playback transition.
    if (m_impl->frameScheduler.state() != CefExternalFrameScheduler::State::Draining) {
      host->Invalidate(PET_VIEW);
      m_impl->requestExternalBeginFrame(true);
    }
  } else {
    m_impl->backgroundFrameHeartbeat.stop();
    m_impl->beginFrameAckWatchdog.stop();
    const bool abortAlreadyPending = m_impl->frameScheduler.state() == CefExternalFrameScheduler::State::Draining;
    const bool abortRequired = m_impl->frameScheduler.suspend();
    if (abortRequired) {
      if (!abortAlreadyPending && !host->AbortPendingExternalBeginFrame()) {
        kLog.error("CEF rejected external BeginFrame abort while parking inactive playback");
        m_impl->frameScheduler.forceSuspend();
        host->WasHidden(true);
      }
    } else {
      host->WasHidden(true);
    }
  }

  if (changed) {
    kLog.info("CEF detached playback refresh {}", active ? "enabled at 1 Hz" : "disabled");
  }
}

void CefBrowserSession::onPresentation(const SurfacePresentationFeedback& feedback) { m_impl->onPresentation(feedback); }

bool CefBrowserSession::onFrameOpportunity() {
  const bool forwardedPointerMotion = m_impl->flushPointerMotion();
  if (!m_impl->attached || m_impl->browser == nullptr) {
    return false;
  }
  const auto priorState = m_impl->frameScheduler.state();
  const std::int64_t nowNs = steadyNowNs();
  auto request = forwardedPointerMotion ? m_impl->frameScheduler.requestUrgent(nowNs)
                                        : m_impl->frameScheduler.onFrameOpportunity(nowNs);
  if (request) {
    m_impl->submitExternalBeginFrame(*request);
  } else if (priorState == CefExternalFrameScheduler::State::InFlight) {
    tracy_latency::externalBeginFrameCoalesced();
  }
  return m_impl->frameScheduler.needsFrameOpportunity();
}

void CefService::doMessageLoopWork() {
  if (m_runtime->initialized) {
    NOCTALIA_TRACE_ZONE("CEF message-pump work");
    CefDoMessageLoopWork();
    std::erase_if(m_runtime->sessions, [](const std::shared_ptr<CefBrowserSession>& session) {
      return session != nullptr && session->m_impl->auxiliary && session->m_impl->browser == nullptr;
    });
  }
}

void CefService::setScheduleWorkCallback(std::function<void(std::int64_t)> cb) {
  m_runtime->scheduleWork->set(std::move(cb));
}

bool CefBrowserSession::uploadIfDirty(TextureManager& textures) {
  (void)textures;
  return std::exchange(m_impl->textureChanged, false);
}

TextureHandle CefBrowserSession::currentTexture() const noexcept { return m_impl->texture; }

CefBrowserSessionState CefBrowserSession::state() const noexcept { return m_impl->state; }

bool CefBrowserSession::hasUsableFrame() const noexcept { return m_impl->hasUsableFrame; }

const std::string& CefBrowserSession::lastError() const noexcept { return m_impl->lastError; }

const CefBrowserMediaState& CefBrowserSession::mediaState() const noexcept { return m_impl->mediaState; }

void CefBrowserSession::invalidateGpuTexture() {
  if (m_impl->gpuBridge != nullptr) {
    m_impl->gpuBridge->invalidate();
  }
  m_impl->texture = {};
  m_impl->hasUsableFrame = false;
  m_impl->setState(CefBrowserSessionState::Recovering);
  if (m_impl->browser) {
    m_impl->browser->GetHost()->Invalidate(PET_VIEW);
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefBrowserSession::setFrameReadyCallback(std::function<void()> cb) { m_impl->frameReady = std::move(cb); }

void CefBrowserSession::setFrameOpportunityCallback(std::function<void()> cb) {
  m_impl->frameOpportunity = std::move(cb);
  m_impl->armFrameOpportunity();
}

void CefBrowserSession::setCursorCallback(std::function<void(std::uint32_t)> cb) {
  m_impl->cursorCb = std::move(cb);
}

void CefBrowserSession::setStateCallback(std::function<void(CefBrowserSessionState)> cb) {
  m_impl->stateCb = std::move(cb);
}

void CefBrowserSession::setPermissionRequestCallback(std::function<void(CefBrowserPermissionRequest)> cb) {
  m_impl->permissionRequestCb = std::move(cb);
}

void CefBrowserSession::setContextMenuRequestCallback(std::function<void(CefBrowserContextMenuRequest)> cb) {
  m_impl->contextMenuRequestCb = std::move(cb);
}

void CefBrowserSession::setPopupCreatedCallback(std::function<void(CefBrowserPopupRequest)> cb) {
  m_impl->popupCreatedCb = std::move(cb);
}

void CefBrowserSession::setClosedCallback(std::function<void()> cb) { m_impl->closedCb = std::move(cb); }

void CefBrowserSession::setMediaStateCallback(std::function<void(const CefBrowserMediaState&)> cb) {
  m_impl->mediaStateCb = std::move(cb);
}
