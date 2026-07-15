#include "cef/cef_service.h"

#include "cef/noctalia_cef_app.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "render/core/texture_manager.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits.h>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

#include "cursor-shape-v1-client-protocol.h"

namespace {

constexpr Logger kLog("cef");

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
  bool acceleratedEnabled = false;
  std::string pendingUrl;
  int logicalWidth = 1280;
  int logicalHeight = 720;
  float deviceScale = 1.0f;

  std::mutex frameMutex;
  // Pending zero-copy frame: a dmabuf whose plane fds we duplicated in
  // OnAcceleratedPaint (valid until we import + close them). Guarded by frameMutex.
  bool havePendingDmabuf = false;
  TextureManager::DmabufImage pendingDmabuf;

  TextureHandle texture;

  void closePendingDmabufLocked() {
    if (!havePendingDmabuf) {
      return;
    }
    for (int i = 0; i < pendingDmabuf.planeCount && i < 4; ++i) {
      if (pendingDmabuf.planes[i].fd >= 0) {
        ::close(pendingDmabuf.planes[i].fd);
        pendingDmabuf.planes[i].fd = -1;
      }
    }
    havePendingDmabuf = false;
  }

  std::function<void(std::int64_t)> scheduleWork;
  std::function<void()> frameReady;
  std::function<void(std::uint32_t)> cursorCb;

  // Guards deferred main-thread callbacks against use-after-free during shutdown.
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

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
    kLog.error("CEF delivered a CPU OSR frame despite shared textures being required");
  }

  // Zero-copy path: CEF hands us a GPU dmabuf. Its fds are valid only for the
  // duration of this call ("released to the pool on return"), so we dup them and
  // import on the main thread. CEF rotates its buffer pool, so a one-frame
  // consume latency is safe.
  void OnAcceleratedPaint(
      CefRefPtr<CefBrowser> /*browser*/, PaintElementType type, const RectList& /*dirtyRects*/,
      const CefAcceleratedPaintInfo& info
  ) override {
    if (type != PET_VIEW) {
      return;
    }
    TextureManager::DmabufImage img;
    img.width = info.extra.coded_size.width > 0
        ? info.extra.coded_size.width
        : static_cast<int>(static_cast<float>(m_impl->logicalWidth) * m_impl->deviceScale);
    img.height = info.extra.coded_size.height > 0
        ? info.extra.coded_size.height
        : static_cast<int>(static_cast<float>(m_impl->logicalHeight) * m_impl->deviceScale);
    img.fourcc = drmFourccFromCef(info.format);
    img.modifier = info.modifier;
    img.hasModifier = true;
    img.planeCount = std::min(info.plane_count, 4);
    for (int i = 0; i < img.planeCount; ++i) {
      img.planes[i].fd = ::dup(info.planes[i].fd);
      if (img.planes[i].fd < 0) {
        for (int j = 0; j < i; ++j) {
          ::close(img.planes[j].fd);
        }
        kLog.error("failed to duplicate CEF dmabuf plane fd");
        return;
      }
      img.planes[i].stride = info.planes[i].stride;
      img.planes[i].offset = info.planes[i].offset;
    }
    {
      std::scoped_lock lock(m_impl->frameMutex);
      m_impl->closePendingDmabufLocked(); // drop an unconsumed older frame's fds
      m_impl->pendingDmabuf = img;
      m_impl->havePendingDmabuf = true;
    }
    auto alive = m_impl->alive;
    auto* impl = m_impl;
    DeferredCall::callLater([alive, impl]() {
      if (alive->load() && impl->frameReady) {
        impl->frameReady();
      }
    });
  }

  bool OnCursorChange(
      CefRefPtr<CefBrowser> /*browser*/, CefCursorHandle /*cursor*/, cef_cursor_type_t type,
      const CefCursorInfo& /*info*/
  ) override {
    const std::uint32_t shape = cursorShapeFromCef(type);
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
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override { m_impl->browser = nullptr; }

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
  if (!m_impl->acceleratedEnabled) {
    kLog.error("refusing to initialize CEF: EGL dmabuf import is unavailable");
    return false;
  }

  m_impl->app = new NoctaliaCefApp([alive, impl](std::int64_t delayMs) {
    // OnScheduleMessagePumpWork can fire on any CEF thread — marshal.
    if (alive->load() && impl->scheduleWork) {
      impl->scheduleWork(delayMs);
    }
  });

  CefSettings settings;
  settings.no_sandbox = true;
  settings.windowless_rendering_enabled = true;
  settings.multi_threaded_message_loop = false;
  settings.external_message_pump = true;
  settings.log_severity = LOGSEVERITY_WARNING;
  CefString(&settings.root_cache_path).FromString(userCachePath());
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
  return true;
}

void CefService::shutdown() {
  {
    std::scoped_lock lock(m_impl->frameMutex);
    m_impl->closePendingDmabufLocked();
  }
  if (!m_impl->initialized) {
    return;
  }
  m_impl->alive->store(false);
  if (m_impl->browser) {
    m_impl->browser->GetHost()->CloseBrowser(true);
    // Pump until the browser finishes closing.
    for (int i = 0; i < 50 && m_impl->browser != nullptr; ++i) {
      CefDoMessageLoopWork();
    }
  }
  m_impl->client = nullptr;
  CefShutdown();
  m_impl->app = nullptr;
  m_impl->initialized = false;
}

void CefService::ensureBrowser(int logicalWidth, int logicalHeight) {
  m_impl->logicalWidth = logicalWidth > 0 ? logicalWidth : m_impl->logicalWidth;
  m_impl->logicalHeight = logicalHeight > 0 ? logicalHeight : m_impl->logicalHeight;
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
  CefBrowserSettings browserSettings;
  browserSettings.windowless_frame_rate = 60;

  const std::string url = m_impl->pendingUrl.empty() ? std::string("about:blank") : m_impl->pendingUrl;
  CefBrowserHost::CreateBrowser(windowInfo, m_impl->client, url, browserSettings, nullptr, nullptr);
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
  }
}

void CefService::navigate(const std::string& url) {
  m_impl->pendingUrl = url;
  if (m_impl->browser) {
    m_impl->browser->GetMainFrame()->LoadURL(url);
  }
}

void CefService::execJs(const std::string& code) {
  if (m_impl->browser) {
    m_impl->browser->GetMainFrame()->ExecuteJavaScript(code, m_impl->browser->GetMainFrame()->GetURL(), 0);
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
  }
}

void CefService::setAcceleratedEnabled(bool enabled) {
  if (m_impl->initialized || m_impl->browser) {
    return;
  }
  m_impl->acceleratedEnabled = enabled;
}

bool CefService::acceleratedEnabled() const noexcept {
  return m_impl->acceleratedEnabled;
}

void CefService::sendMouseMove(float x, float y, std::uint32_t modifiers, bool leaving) {
  if (!m_impl->browser) {
    return;
  }
  CefMouseEvent event;
  event.x = static_cast<int>(x);
  event.y = static_cast<int>(y);
  event.modifiers = cefModifiersFromKeyMod(modifiers);
  m_impl->browser->GetHost()->SendMouseMoveEvent(event, leaving);
}

void CefService::sendMouseButton(
    float x, float y, int button, bool pressed, int clickCount, std::uint32_t modifiers
) {
  if (!m_impl->browser) {
    return;
  }
  CefMouseEvent event;
  event.x = static_cast<int>(x);
  event.y = static_cast<int>(y);
  event.modifiers = cefModifiersFromKeyMod(modifiers);
  cef_mouse_button_type_t type = MBT_LEFT;
  if (button == 1) {
    type = MBT_MIDDLE;
  } else if (button == 2) {
    type = MBT_RIGHT;
  }
  m_impl->browser->GetHost()->SendMouseClickEvent(event, type, !pressed, clickCount);
}

void CefService::sendMouseWheel(float x, float y, float deltaX, float deltaY, std::uint32_t modifiers) {
  if (!m_impl->browser) {
    return;
  }
  CefMouseEvent event;
  event.x = static_cast<int>(x);
  event.y = static_cast<int>(y);
  event.modifiers = cefModifiersFromKeyMod(modifiers);
  m_impl->browser->GetHost()->SendMouseWheelEvent(event, static_cast<int>(deltaX), static_cast<int>(deltaY));
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
}

void CefService::setFocus(bool focused) {
  if (m_impl->browser) {
    m_impl->browser->GetHost()->SetFocus(focused);
  }
}

void CefService::setDisplayAttached(bool attached) {
  if (m_impl->attached == attached) {
    return;
  }
  m_impl->attached = attached;
  if (!m_impl->browser) {
    return;
  }
  m_impl->browser->GetHost()->WasHidden(!attached);
  if (attached) {
    m_impl->browser->GetHost()->Invalidate(PET_VIEW);
    m_impl->browser->GetHost()->SetFocus(true);
  }
}

void CefService::doMessageLoopWork() {
  if (m_impl->initialized) {
    CefDoMessageLoopWork();
  }
}

void CefService::setScheduleWorkCallback(std::function<void(std::int64_t)> cb) {
  m_impl->scheduleWork = std::move(cb);
}

bool CefService::uploadIfDirty(TextureManager& textures) {
  TextureManager::DmabufImage image;
  {
    std::scoped_lock lock(m_impl->frameMutex);
    if (!m_impl->havePendingDmabuf) {
      return false;
    }
    image = m_impl->pendingDmabuf;
    m_impl->pendingDmabuf = {};
    m_impl->havePendingDmabuf = false;
  }

  TextureHandle next = textures.importDmabuf(image);
  for (int i = 0; i < image.planeCount && i < 4; ++i) {
    if (image.planes[i].fd >= 0) {
      ::close(image.planes[i].fd);
    }
  }
  if (!next.valid()) {
    kLog.error("failed to import CEF dmabuf frame as an EGLImage");
    return false;
  }
  if (m_impl->texture.valid()) {
    textures.unload(m_impl->texture);
  }
  m_impl->texture = next;
  return true;
}

TextureHandle CefService::currentTexture() const noexcept {
  return m_impl->texture;
}

void CefService::invalidateGpuTexture() {
  // The GL name and backing EGLImage are gone after context loss. A requested
  // CEF repaint supplies a fresh dmabuf for the new context.
  m_impl->texture = {};
  if (m_impl->browser) {
    m_impl->browser->GetHost()->Invalidate(PET_VIEW);
  }
}

void CefService::setFrameReadyCallback(std::function<void()> cb) {
  m_impl->frameReady = std::move(cb);
}

void CefService::setCursorCallback(std::function<void(std::uint32_t)> cb) {
  m_impl->cursorCb = std::move(cb);
}
