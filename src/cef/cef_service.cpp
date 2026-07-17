#include "cef/cef_service.h"

#include "cef/cef_external_frame_scheduler.h"
#include "cef/cef_gpu_frame_bridge.h"
#include "cef/cef_pointer_motion_coalescer.h"
#include "cef/noctalia_cef_app.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "core/tracy.h"
#include "core/tracy_latency.h"
#include "render/core/texture_manager.h"
#include "render/graphics_device.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>

#include "cursor-shape-v1-client-protocol.h"

namespace {

constexpr Logger kLog("cef");
constexpr int kCefWindowlessFrameRate = 120;
constexpr std::int64_t kFallbackBeginFrameIntervalNs = 1'000'000'000LL / kCefWindowlessFrameRate;
constexpr std::int64_t kBeginFrameAckRecoveryNs = 2'000'000'000;
constexpr std::int64_t kIdleAnimationProbeNs = 250'000'000;

std::int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()
  ).count();
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
    case 0xff08: return 0x08; // BackSpace -> VK_BACK
    case 0xff09: return 0x09; // Tab -> VK_TAB
    case 0xff0d: return 0x0D; // Return -> VK_RETURN
    case 0xff1b: return 0x1B; // Escape -> VK_ESCAPE
    case 0xff50: return 0x24; // Home
    case 0xff51: return 0x25; // Left
    case 0xff52: return 0x26; // Up
    case 0xff53: return 0x27; // Right
    case 0xff54: return 0x28; // Down
    case 0xff55: return 0x21; // Page_Up
    case 0xff56: return 0x22; // Page_Down
    case 0xff57: return 0x23; // End
    case 0xffff: return 0x2E; // Delete
    default: break;
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
    return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8)
        | (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
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

} // namespace

class NoctaliaExternalBeginFrameCallback : public CefExternalBeginFrameCallback {
public:
  NoctaliaExternalBeginFrameCallback(
      CefService::Impl* impl, std::shared_ptr<std::atomic<bool>> alive,
      std::uint64_t requestId, std::uint64_t generation
  )
      : m_impl(impl),
        m_alive(std::move(alive)),
        m_requestId(requestId),
        m_generation(generation) {}

  void OnComplete(bool hasDamage) override;

private:
  CefService::Impl* m_impl;
  std::shared_ptr<std::atomic<bool>> m_alive;
  std::uint64_t m_requestId;
  std::uint64_t m_generation;

  IMPLEMENT_REFCOUNTING(NoctaliaExternalBeginFrameCallback);
};

// ---------------------------------------------------------------------------
// Impl: the actual CEF-owning state.
// ---------------------------------------------------------------------------
struct CefService::Impl {
  std::string cefDir;
  std::string helperPath;

  CefRefPtr<NoctaliaCefApp> app;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefClient> client;

  bool initialized = false;
  bool attached = false;
  std::uint32_t presentationRefreshNs = 0;
  int configuredFrameRate = kCefWindowlessFrameRate;
  CefExternalFrameScheduler frameScheduler{kFallbackBeginFrameIntervalNs};
  Timer beginFrameAckWatchdog;
  Timer idleAnimationProbe;
  std::string pendingUrl;
  int logicalWidth = 1280;
  int logicalHeight = 720;
  float deviceScale = 1.0f;

  std::unique_ptr<CefGpuFrameBridge> gpuBridge;
  std::atomic<bool> loggedCpuPaint = false;
  std::atomic<bool> loggedAcceleratedPaint = false;
  TextureHandle texture;
  bool textureChanged = false;
  std::uint64_t supersededReadyFrames = 0;
  CefPointerMotionCoalescer pointerMotion;
  std::uint32_t pointerButtonFlags = 0;

  std::function<void(std::int64_t)> scheduleWork;
  std::function<void()> frameReady;
  std::function<void(std::uint32_t)> cursorCb;
  int lastCursorType = -1;
  std::uint32_t lastCursorShape = 0;

  // Guards deferred main-thread callbacks against use-after-free during shutdown.
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

  void submitExternalBeginFrame(const CefExternalFrameScheduler::Request& request) {
    if (!attached || browser == nullptr || request.generation != frameScheduler.generation()) {
      frameScheduler.abandon(request.id);
      return;
    }

    CefExternalBeginFrameArgs args;
    args.deadline_delta_ns = request.deadlineDeltaNs;
    args.interval_ns = request.intervalNs;
    CefRefPtr<CefExternalBeginFrameCallback> callback =
        new NoctaliaExternalBeginFrameCallback(
            this, alive, request.id, request.generation
        );
    const bool accepted = browser->GetHost()->SendExternalBeginFrameWithTiming(args, callback);
    if (!accepted) {
      frameScheduler.abandon(request.id);
      kLog.warn(
          "CEF rejected timed external begin frame {} (generation {})",
          request.id, request.generation
      );
      NOCTALIA_TRACE_PLOT("CEF begin frame outstanding", static_cast<std::int64_t>(0));
      return;
    }
    // The CEF contract is asynchronous, but keep the client correct if an
    // implementation can complete inline and re-enter with the next request.
    if (!frameScheduler.isInFlight(request.id)) {
      return;
    }

    idleAnimationProbe.stop();
    tracy_latency::externalBeginFrameIssued(request.urgent);
    NOCTALIA_TRACE_PLOT("CEF external begin frames", static_cast<std::int64_t>(1));
    NOCTALIA_TRACE_PLOT("CEF begin frame outstanding", static_cast<std::int64_t>(1));
    auto token = alive;
    beginFrameAckWatchdog.stop();
    beginFrameAckWatchdog.start(
        std::chrono::milliseconds(kBeginFrameAckRecoveryNs / 1'000'000),
        [this, token, request]() {
          if (!token->load() || request.generation != frameScheduler.generation()
              || !frameScheduler.isInFlight(request.id)) {
            return;
          }
          kLog.error(
              "CEF external begin frame {} was not acknowledged within the recovery deadline; "
              "preserving the accepted one-in-flight state",
              request.id
          );
          if (attached && browser != nullptr) {
            browser->GetHost()->Invalidate(PET_VIEW);
          }
        }
    );
  }

  void scheduleIdleProbe() {
    idleAnimationProbe.stop();
    if (!attached || browser == nullptr || !frameScheduler.idleSuppressed()) {
      return;
    }
    auto token = alive;
    const std::uint64_t generation = frameScheduler.generation();
    idleAnimationProbe.start(
        std::chrono::milliseconds(kIdleAnimationProbeNs / 1'000'000),
        [this, token, generation]() {
          if (!token->load() || !attached || browser == nullptr
              || generation != frameScheduler.generation()) {
            return;
          }
          if (auto request = frameScheduler.requestIdleProbe(steadyNowNs())) {
            submitExternalBeginFrame(*request);
          }
        }
    );
  }

  void onExternalBeginFrameAck(
      std::uint64_t requestId, std::uint64_t generation, bool hasDamage
  ) {
    if (generation != frameScheduler.generation() || !frameScheduler.isInFlight(requestId)) {
      return;
    }
    beginFrameAckWatchdog.stop();
    auto next = frameScheduler.acknowledge(requestId, hasDamage, steadyNowNs());
    NOCTALIA_TRACE_PLOT("CEF begin frame outstanding", static_cast<std::int64_t>(next.has_value()));
    NOCTALIA_TRACE_PLOT(
        "CEF no-damage begin frames",
        static_cast<std::int64_t>(frameScheduler.consecutiveNoDamage())
    );
    if (next) {
      submitExternalBeginFrame(*next);
    } else {
      scheduleIdleProbe();
    }
  }

  void requestExternalBeginFrame(bool urgent) {
    if (!attached || browser == nullptr) {
      return;
    }
    const auto priorState = frameScheduler.state();
    auto request = urgent
        ? frameScheduler.requestUrgent(steadyNowNs())
        : frameScheduler.requestNormal(steadyNowNs());
    if (request) {
      submitExternalBeginFrame(*request);
    } else if (priorState == CefExternalFrameScheduler::State::InFlight) {
      tracy_latency::externalBeginFrameCoalesced();
    }
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
      const int frameRate = std::max(
          1, static_cast<int>((1'000'000'000ULL + presentationRefreshNs / 2U) / presentationRefreshNs)
      );
      if (browser != nullptr && frameRate != configuredFrameRate) {
        configuredFrameRate = frameRate;
        browser->GetHost()->SetWindowlessFrameRate(frameRate);
        kLog.info("CEF external begin-frame rate updated from presentation feedback: {} fps", frameRate);
      }
    }
    NOCTALIA_TRACE_PLOT("CEF surface refresh ns", static_cast<std::int64_t>(presentationRefreshNs));
  }

  void startExternalScheduler() {
    if (!attached || browser == nullptr) {
      return;
    }
    if (frameScheduler.state() == CefExternalFrameScheduler::State::Suspended) {
      frameScheduler.resume();
    }
    requestExternalBeginFrame(false);
  }

  void stopExternalScheduler() {
    beginFrameAckWatchdog.stop();
    idleAnimationProbe.stop();
    frameScheduler.suspend();
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
                          public CefDisplayHandler {
public:
  explicit NoctaliaCefClient(CefService::Impl* impl) : m_impl(impl) {}

  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

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
      kLog.error("CEF delivered a CPU OSR frame despite shared textures being required ({}x{})", width, height);
    }
  }

  // CEF's fds are borrowed for this callback only. The bridge duplicates a
  // DMA-BUF only on an import-cache miss. The normal path samples that cached
  // import directly and returns a release fence after Graphite submission;
  // sampling submission returns a token-correlated release fence to CEF.
  void OnAcceleratedPaint(
      CefRefPtr<CefBrowser> /*browser*/, PaintElementType type, const RectList& /*dirtyRects*/,
      const CefAcceleratedPaintInfo& info
  ) override {
    NOCTALIA_TRACE_ZONE("CEF OnAcceleratedPaint");
    CEF_REQUIRE_UI_THREAD();
    if (type != PET_VIEW) {
      return;
    }
    if (m_impl->gpuBridge == nullptr) {
      kLog.error("received accelerated CEF frame before the Vulkan bridge was attached");
      return;
    }
    tracy_latency::acceleratedPaintArrived();
    BorrowedDmabufFrame frame;
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
    frame.fourcc = drmFourccFromCef(info.format);
    frame.modifier = info.modifier;
    frame.acquireFenceFd = info.acquire_fence_fd;
    frame.planeCount = std::min(info.plane_count, 4);
    if (!m_impl->loggedAcceleratedPaint.exchange(true)) {
      kLog.info(
          "received first accelerated CEF frame: {}x{}, fourcc=0x{:08x}, modifier=0x{:016x}, planes={}",
          frame.width, frame.height, frame.fourcc, frame.modifier, frame.planeCount
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
      m_impl->requestExternalBeginFrame(true);
      return;
    }
    NOCTALIA_TRACE_FRAME("CEF accelerated frame accepted");
    NOCTALIA_TRACE_PLOT(
        "CEF frames accepted", static_cast<std::int64_t>(m_impl->gpuBridge->acceptedFrameCount())
    );
    NOCTALIA_TRACE_PLOT("CEF frame width", static_cast<std::int64_t>(frame.width));
    NOCTALIA_TRACE_PLOT("CEF frame height", static_cast<std::int64_t>(frame.height));
    if (m_impl->textureChanged) {
      ++m_impl->supersededReadyFrames;
      NOCTALIA_TRACE_PLOT(
          "CEF ready frames superseded before adoption",
          static_cast<std::int64_t>(m_impl->supersededReadyFrames)
      );
    }
    m_impl->texture = m_impl->gpuBridge->texture();
    m_impl->textureChanged = true;
    if (m_impl->frameReady) {
      m_impl->frameReady();
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
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    m_impl->browser = browser;
    browser->GetHost()->WasHidden(!m_impl->attached);
    browser->GetHost()->WasResized();
    if (m_impl->attached) {
      browser->GetHost()->Invalidate(PET_VIEW);
      browser->GetHost()->SetFocus(true);
    }
    m_impl->startExternalScheduler();
    kLog.info("CEF browser created");
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override {
    m_impl->stopExternalScheduler();
    m_impl->browser = nullptr;
  }

  // CefLoadHandler
  void OnLoadingStateChange(
      CefRefPtr<CefBrowser> /*browser*/, bool isLoading, bool /*canGoBack*/, bool /*canGoForward*/
  ) override {
    kLog.info("CEF loading state changed: {}", isLoading ? "loading" : "idle");
    m_impl->requestExternalBeginFrame(isLoading);
  }

  void OnLoadEnd(
      CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, int httpStatusCode
  ) override {
    if (frame->IsMain()) {
      kLog.info("CEF main frame loaded: status={} url={}", httpStatusCode, frame->GetURL().ToString());
    }
  }

  void OnLoadError(
      CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
      const CefString& errorText, const CefString& failedUrl
  ) override {
    if (frame->IsMain()) {
      kLog.error(
          "CEF main frame load failed: code={} error={} url={}", static_cast<int>(errorCode),
          errorText.ToString(), failedUrl.ToString()
      );
    }
  }

private:
  CefService::Impl* m_impl;
  IMPLEMENT_REFCOUNTING(NoctaliaCefClient);
};

} // namespace

// ---------------------------------------------------------------------------
// CefService
// ---------------------------------------------------------------------------
CefService::CefService(std::string cefDir, std::string helperPath) : m_impl(std::make_unique<Impl>()) {
  m_impl->cefDir = std::move(cefDir);
  m_impl->helperPath = helperPath.empty() ? helperNextToSelf() : std::move(helperPath);
}

CefService::~CefService() {
  shutdown();
}

bool CefService::initialized() const noexcept {
  return m_impl->initialized;
}

bool CefService::initialize() {
  if (m_impl->initialized) {
    return true;
  }

  CefMainArgs mainArgs(0, nullptr);

  auto* impl = m_impl.get();
  auto alive = m_impl->alive;
  m_impl->app = new NoctaliaCefApp([alive, impl](std::int64_t delayMs) {
    // OnScheduleMessagePumpWork can fire on any CEF thread — marshal.
    if (alive->load() && impl->scheduleWork) {
      impl->scheduleWork(delayMs);
    }
  });

  const std::string rootCachePath = userCachePath();
  if (const char* widevine = std::getenv("NOCTALIA_CEF_WIDEVINE");
      widevine != nullptr && widevine[0] != '\0') {
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
  // Apple Music uses session cookies for part of its authentication state.
  // Keep those cookies in the persistent CEF profile across shell restarts.
  settings.persist_session_cookies = true;
  // A root_cache_path alone does not create a persistent request context.
  // Reuse the existing root as the global profile so enabling persistence does
  // not relocate or discard the current Apple Music browser data.
  CefString(&settings.root_cache_path).FromString(rootCachePath);
  CefString(&settings.cache_path).FromString(rootCachePath);
  CefString(&settings.resources_dir_path).FromString(m_impl->cefDir + "/Resources");
  CefString(&settings.locales_dir_path).FromString(m_impl->cefDir + "/Resources/locales");
  if (!m_impl->helperPath.empty()) {
    CefString(&settings.browser_subprocess_path).FromString(m_impl->helperPath);
  }

  if (!CefInitialize(mainArgs, settings, m_impl->app.get(), nullptr)) {
    m_impl->app = nullptr;
    return false;
  }
  m_impl->initialized = true;
  kLog.info("CEF persistent profile enabled at {}", rootCachePath);
  return true;
}

void CefService::attachGraphicsDevice(GraphicsDevice& graphics) {
  if (!graphics.valid() || !graphics.cefExternalMemoryEnabled()) {
    throw std::runtime_error("CEF requires the Vulkan external-memory GraphicsDevice contract");
  }
  m_impl->gpuBridge = std::make_unique<CefGpuFrameBridge>(
      graphics, &graphics.textureManager(),
      [this](std::int64_t captureCounter, int fd) {
        if (m_impl->browser == nullptr) {
          kLog.warn("dropping CEF release fence for closed browser frame {}", captureCounter);
          return;
        }
        m_impl->browser->GetHost()->SetAcceleratedPaintReleaseFence(captureCounter, fd);
      }
  );
}

void CefService::prepareForGraphicsDeviceRebuild() {
  m_impl->stopExternalScheduler();
  if (m_impl->gpuBridge != nullptr) {
    m_impl->gpuBridge->abandonDevice();
    m_impl->gpuBridge.reset();
  }
  m_impl->texture = {};
  m_impl->textureChanged = true;
}

void CefService::resumeAfterGraphicsDeviceRebuild(GraphicsDevice& graphics) {
  attachGraphicsDevice(graphics);
  m_impl->texture = {};
  m_impl->textureChanged = true;
  if (m_impl->browser != nullptr) {
    m_impl->browser->GetHost()->Invalidate(PET_VIEW);
    m_impl->browser->GetHost()->WasResized();
    m_impl->startExternalScheduler();
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefService::shutdown() {
  if (!m_impl->initialized) {
    return;
  }
  m_impl->alive->store(false);
  m_impl->stopExternalScheduler();
  if (m_impl->browser) {
    m_impl->browser->GetHost()->CloseBrowser(true);
    // OnBeforeClose is the CEF contract that all browser-owned objects have
    // been released. Give Chromium's child processes real wall-clock time to
    // drain instead of spinning through 50 message-pump calls immediately.
    for (int i = 0; i < 500 && m_impl->browser != nullptr; ++i) {
      CefDoMessageLoopWork();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (m_impl->browser != nullptr) {
      kLog.error("CEF browser did not reach OnBeforeClose before shutdown");
    }
  }
  m_impl->client = nullptr;
  if (m_impl->gpuBridge != nullptr) {
    const CefGpuFrameBridgeStats stats = m_impl->gpuBridge->stats();
    kLog.info(
        "CEF GPU bridge summary: accepted={}, directStaged={}, "
        "directSampled={}, directDiscarded={}, releaseFences={}, imports={}/{}, "
        "activeImports={}, cacheHits={}, cacheMisses={}",
        stats.framesAccepted, stats.directFramesStaged,
        stats.directFramesSampled, stats.directFramesDiscarded,
        stats.releaseFenceFdsExported, stats.importsCreated, stats.importsDestroyed,
        stats.activeImports, stats.importCacheHits, stats.importCacheMisses
    );
  }
  m_impl->gpuBridge.reset();
  CefShutdown();
  m_impl->app = nullptr;
  m_impl->initialized = false;
}

void CefService::ensureBrowser(int logicalWidth, int logicalHeight) {
  m_impl->logicalWidth = logicalWidth > 0 ? logicalWidth : m_impl->logicalWidth;
  m_impl->logicalHeight = logicalHeight > 0 ? logicalHeight : m_impl->logicalHeight;
  if (m_impl->gpuBridge == nullptr) {
    kLog.error("refusing to create CEF browser: mandatory Vulkan DMA-BUF bridge is unavailable");
    return;
  }
  if (!m_impl->initialized && !initialize()) {
    return;
  }
  if (m_impl->browser) {
    return;
  }
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
    kLog.info("CEF browser creation requested: {}x{} fps={} scheduler={} url={}",
              m_impl->logicalWidth,
              m_impl->logicalHeight,
              kCefWindowlessFrameRate,
              "wayland-frame-callback",
              url);
  }
}

void CefService::resize(int logicalWidth, int logicalHeight) {
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

void CefService::navigate(const std::string& url) {
  m_impl->pendingUrl = url;
  if (m_impl->browser) {
    m_impl->browser->GetMainFrame()->LoadURL(url);
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefService::execJs(const std::string& code) {
  if (m_impl->browser) {
    m_impl->browser->GetMainFrame()->ExecuteJavaScript(code, m_impl->browser->GetMainFrame()->GetURL(), 0);
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefService::setDeviceScale(float scale) {
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

void CefService::sendMouseMove(float x, float y, std::uint32_t modifiers, bool leaving) {
  if (!m_impl->browser) {
    return;
  }
  if (!leaving) {
    m_impl->pointerMotion.queue({static_cast<int>(x), static_cast<int>(y), modifiers});
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

void CefService::flushMouseMove() {
  if (m_impl->flushPointerMotion()) {
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefService::sendMouseButton(
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

void CefService::sendMouseWheel(float x, float y, float deltaX, float deltaY, std::uint32_t modifiers) {
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

void CefService::sendKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed) {
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

void CefService::setFocus(bool focused) {
  if (m_impl->browser) {
    m_impl->browser->GetHost()->SetFocus(focused);
    if (focused) {
      m_impl->requestExternalBeginFrame(true);
    }
  }
}

void CefService::setDisplayAttached(bool attached) {
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
  host->WasHidden(!attached);
  if (attached) {
    host->Invalidate(PET_VIEW);
    host->SetFocus(true);
    m_impl->startExternalScheduler();
  } else {
    m_impl->stopExternalScheduler();
    if (m_impl->gpuBridge != nullptr) {
      m_impl->gpuBridge->discardPendingFrame();
    }
  }
}

void CefService::onPresentation(const SurfacePresentationFeedback& feedback) {
  m_impl->onPresentation(feedback);
}

void CefService::onFrameOpportunity() {
  const bool forwardedPointerMotion = m_impl->flushPointerMotion();
  if (!m_impl->attached || m_impl->browser == nullptr) {
    return;
  }
  const auto priorState = m_impl->frameScheduler.state();
  const std::int64_t nowNs = steadyNowNs();
  auto request = forwardedPointerMotion
      ? m_impl->frameScheduler.requestUrgent(nowNs)
      : m_impl->frameScheduler.onFrameOpportunity(nowNs);
  if (request) {
    m_impl->submitExternalBeginFrame(*request);
  } else if (priorState == CefExternalFrameScheduler::State::InFlight) {
    tracy_latency::externalBeginFrameCoalesced();
  }
}

void CefService::doMessageLoopWork() {
  if (m_impl->initialized) {
    NOCTALIA_TRACE_ZONE("CEF message-pump work");
    CefDoMessageLoopWork();
  }
}

void CefService::setScheduleWorkCallback(std::function<void(std::int64_t)> cb) {
  m_impl->scheduleWork = std::move(cb);
}

bool CefService::uploadIfDirty(TextureManager& textures) {
  (void)textures;
  return std::exchange(m_impl->textureChanged, false);
}

TextureHandle CefService::currentTexture() const noexcept {
  return m_impl->texture;
}

void CefService::invalidateGpuTexture() {
  if (m_impl->gpuBridge != nullptr) {
    m_impl->gpuBridge->invalidate();
  }
  m_impl->texture = {};
  if (m_impl->browser) {
    m_impl->browser->GetHost()->Invalidate(PET_VIEW);
    m_impl->requestExternalBeginFrame(true);
  }
}

void CefService::setFrameReadyCallback(std::function<void()> cb) {
  m_impl->frameReady = std::move(cb);
}

void CefService::setCursorCallback(std::function<void(std::uint32_t)> cb) {
  m_impl->cursorCb = std::move(cb);
}
