#pragma once

#include "render/core/texture_handle.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class TextureManager;

// App-level owner of the embedded CEF browser. Deliberately exposes NO CEF
// types (pImpl) so the rest of the shell — panels, the surface node, the bar
// widget — never pulls the CEF headers. The browser and its GL texture live
// here for the process lifetime, surviving panel open/close and scene rebuilds
// (panels destroy their surface + scene nodes on every close), so audio keeps
// playing and reopening is instant.
//
// Threading: OnAcceleratedPaint and OnScheduleMessagePumpWork fire on CEF
// threads and only duplicate handles / marshal to the main loop. Every method here is
// called on noctalia's main thread; all GL happens on the main thread during
// the surface node's update (same call site as the audio visualizers).
class CefService {
public:
  // cefDir: the distribution root (contains Release/ and Resources/).
  // helperPath: the CEF subprocess binary (noctalia_cef_helper). If empty it is
  // resolved next to the running executable.
  explicit CefService(std::string cefDir, std::string helperPath = {});
  ~CefService();

  CefService(const CefService&) = delete;
  CefService& operator=(const CefService&) = delete;

  // Lazily initialize CEF (first browser use). Returns false on failure.
  bool initialize();
  void shutdown();
  [[nodiscard]] bool initialized() const noexcept;

  // Browser lifecycle. Sizes are in logical (DIP) units; the device scale set
  // via setDeviceScale() determines the pixel buffer size CEF paints.
  void ensureBrowser(int logicalWidth, int logicalHeight);
  void resize(int logicalWidth, int logicalHeight);
  void navigate(const std::string& url);
  void execJs(const std::string& code);
  void setDeviceScale(float scale);

  // Declare whether the renderer can import dmabufs. Must be set before CEF is
  // initialized. Browser creation is refused when unavailable; production CEF
  // rendering has no CPU paint fallback.
  void setAcceleratedEnabled(bool enabled);
  [[nodiscard]] bool acceleratedEnabled() const noexcept;

  // Input — logical (DIP) coordinates, matching CefMouseEvent's coordinate space.
  void sendMouseMove(float x, float y, std::uint32_t modifiers, bool leaving = false);
  void sendMouseButton(float x, float y, int button, bool pressed, int clickCount, std::uint32_t modifiers);
  void sendMouseWheel(float x, float y, float deltaX, float deltaY, std::uint32_t modifiers);
  void sendKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed);
  void setFocus(bool focused);

  // Keep the browser alive but stop painting when its display is detached
  // (panel closed); repaint + focus on re-attach.
  void setDisplayAttached(bool attached);

  // Message pump — driven by CefPollSource on the main thread.
  void doMessageLoopWork();
  void setScheduleWorkCallback(std::function<void(std::int64_t delayMs)> cb);

  // Texture bridge — call on the main thread with the GL context current (from
  // the surface node's update). Imports the latest dmabuf frame if one arrived.
  bool uploadIfDirty(TextureManager& textures);
  [[nodiscard]] TextureHandle currentTexture() const noexcept;
  // GPU-loss / scene rebuild: drop the texture handle and force a repaint so
  // the next frame re-creates it on the fresh context.
  void invalidateGpuTexture();

  // Invoked on the main thread when a fresh frame has been buffered (schedule a
  // redraw) and when the page cursor changes (arg: WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_*).
  void setFrameReadyCallback(std::function<void()> cb);
  void setCursorCallback(std::function<void(std::uint32_t shape)> cb);

  // Opaque implementation holding all CEF state — defined in the .cpp. Public so
  // the .cpp-local CEF client can reference it; stays an incomplete type here,
  // so no CEF headers leak to includers.
  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
};
