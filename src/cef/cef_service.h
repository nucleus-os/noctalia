#pragma once

#include "render/core/texture_handle.h"
#include "render/presentation_timing.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class TextureManager;
class GraphicsDevice;
class CefBrowserSession;

// Process-wide owner of CEF initialization, the external message pump, and the
// live browser-session registry. Deliberately exposes no CEF types; each
// CefBrowserSession privately owns its browser, scheduler, Vulkan bridge, and
// stable texture while panels and scene nodes consume only the CEF-free API.
//
// Threading: the external CEF message pump, acknowledged BeginFrame scheduler,
// accelerated DMA-BUF acceptance, Graphite texture rebinding, and public
// methods all run on Noctalia's main thread. OnScheduleMessagePumpWork may fire
// on another CEF thread and is marshalled to that loop.
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
  // Return the unique top-level browser session for this code-defined ID.
  [[nodiscard]] std::shared_ptr<CefBrowserSession> createBrowserSession(std::string id);
  // Bind the mandatory process-wide Vulkan/Graphite device after CEF has
  // established its allocator. Browser creation is refused until this bridge
  // exists; there is no GLES or CPU rendering path.
  void attachGraphicsDevice(GraphicsDevice& graphics);
  void prepareForGraphicsDeviceRebuild();
  void resumeAfterGraphicsDeviceRebuild(GraphicsDevice& graphics);
  void shutdown();
  [[nodiscard]] bool initialized() const noexcept;

  // Message pump — driven by CefPollSource on the main thread.
  void doMessageLoopWork();
  void setScheduleWorkCallback(std::function<void(std::int64_t delayMs)> cb);

  struct Runtime;

private:
  std::unique_ptr<Runtime> m_runtime;
};
