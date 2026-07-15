#pragma once

#include "render/core/texture_handle.h"
#include "render/presentation_timing.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class TextureManager;
class GraphicsDevice;

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

  // Initialize CEF's process allocator and browser runtime. This may happen
  // before a renderer is attached; ensureBrowser separately enforces that an
  // accelerated DMA-BUF path is available.
  bool initialize();
  // Bind the mandatory process-wide Vulkan/Graphite device after CEF has
  // established its allocator. Browser creation is refused until this bridge
  // exists; there is no GLES or CPU rendering path.
  void attachGraphicsDevice(GraphicsDevice& graphics);
  void prepareForGraphicsDeviceRebuild();
  void resumeAfterGraphicsDeviceRebuild(GraphicsDevice& graphics);
  void shutdown();
  [[nodiscard]] bool initialized() const noexcept;

  // Browser lifecycle. Sizes are in logical (DIP) units; the device scale set
  // via setDeviceScale() determines the pixel buffer size CEF paints.
  void ensureBrowser(int logicalWidth, int logicalHeight);
  void resize(int logicalWidth, int logicalHeight);
  void navigate(const std::string& url);
  void execJs(const std::string& code);
  void setDeviceScale(float scale);

  // Input — logical (DIP) coordinates, matching CefMouseEvent's coordinate space.
  void sendMouseMove(float x, float y, std::uint32_t modifiers, bool leaving = false);
  void sendMouseButton(float x, float y, int button, bool pressed, int clickCount, std::uint32_t modifiers);
  void sendMouseWheel(float x, float y, float deltaX, float deltaY, std::uint32_t modifiers);
  void sendKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed);
  void setFocus(bool focused);

  // Keep the browser alive but stop painting when its display is detached
  // (panel closed); repaint + focus on re-attach.
  void setDisplayAttached(bool attached);
  void onPresentation(const SurfacePresentationFeedback& feedback);

  // Message pump — driven by CefPollSource on the main thread.
  void doMessageLoopWork();
  void setScheduleWorkCallback(std::function<void(std::int64_t delayMs)> cb);

  // Texture bridge — observes whether accelerated paint rebound the stable
  // Graphite texture handle to a newly arrived DMA-BUF image.
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
