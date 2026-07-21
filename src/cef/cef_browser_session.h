#pragma once

#include "render/core/texture_handle.h"
#include "render/presentation_timing.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CefService;
class CefBrowserSession;
class TextureManager;
class GraphicsDevice;

enum class CefBrowserSessionState : std::uint8_t {
  NotCreated,
  Loading,
  Ready,
  Failed,
  Recovering,
  Fatal,
};

enum CefBrowserPermission : std::uint32_t {
  CefBrowserPermissionNone = 0,
  CefBrowserPermissionMicrophone = 1U << 0U,
  CefBrowserPermissionCamera = 1U << 1U,
  CefBrowserPermissionNotifications = 1U << 2U,
  CefBrowserPermissionClipboard = 1U << 3U,
};

struct CefBrowserPermissionRequest {
  std::string origin;
  std::uint32_t permissions = CefBrowserPermissionNone;
  // Resolves each requested permission independently. Bits omitted from the
  // returned mask are denied.
  std::function<void(std::uint32_t allowedPermissions)> resolve;
};

struct CefBrowserMediaState {
  bool available = false;
  bool playing = false;
  std::string title;
  std::string artist;
  std::string album;
  std::string artworkUrl;
};

struct CefBrowserContextMenuEntry {
  std::int32_t commandId = 0;
  std::string label;
  bool enabled = true;
  bool separator = false;
  bool checkmark = false;
  bool radio = false;
  bool checked = false;
};

// CEF-free request boundary for native shell presentation. Completing with a
// command ID asks Chromium to execute that default menu command; nullopt
// dismisses the menu without executing anything.
struct CefBrowserContextMenuRequest {
  int x = 0;
  int y = 0;
  std::vector<CefBrowserContextMenuEntry> entries;
  std::function<void(std::optional<std::int32_t>)> complete;
};

struct CefBrowserPopupRequest {
  std::shared_ptr<CefBrowserSession> session;
  int preferredWidth = 0;
  int preferredHeight = 0;
};

// One windowless browser/view presented by Noctalia. The public boundary is
// deliberately CEF-free so shell panels and scene nodes cannot accidentally
// couple themselves to Chromium types or process-wide runtime ownership.
//
class CefBrowserSession {
public:
  ~CefBrowserSession();
  CefBrowserSession(const CefBrowserSession&) = delete;
  CefBrowserSession& operator=(const CefBrowserSession&) = delete;

  [[nodiscard]] const std::string& id() const noexcept { return m_id; }

  void ensureBrowser(int logicalWidth, int logicalHeight);
  void resize(int logicalWidth, int logicalHeight);
  void navigate(const std::string& url);
  void reload();
  void close();
  void execJs(const std::string& code);
  void preparePresentationResize(std::function<void()> ready);
  void setDeviceScale(float scale);

  void sendMouseMove(float x, float y, std::uint32_t modifiers, bool leaving = false);
  void flushMouseMove();
  void sendMouseButton(float x, float y, int button, bool pressed, int clickCount, std::uint32_t modifiers);
  void sendMouseWheel(float x, float y, float deltaX, float deltaY, std::uint32_t modifiers);
  void sendKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed);
  void goBack();
  void goForward();
  void setFocus(bool focused);

  void setDisplayAttached(bool attached);
  void setBackgroundPlaybackActive(bool active);
  [[nodiscard]] bool onFrameOpportunity();
  void onPresentation(const SurfacePresentationFeedback& feedback);

  bool uploadIfDirty(TextureManager& textures);
  [[nodiscard]] TextureHandle currentTexture() const noexcept;
  [[nodiscard]] CefBrowserSessionState state() const noexcept;
  [[nodiscard]] bool hasUsableFrame() const noexcept;
  [[nodiscard]] const std::string& lastError() const noexcept;
  [[nodiscard]] const CefBrowserMediaState& mediaState() const noexcept;
  void invalidateGpuTexture();

  void setFrameReadyCallback(std::function<void()> cb);
  void setFrameOpportunityCallback(std::function<void()> cb);
  void setCursorCallback(std::function<void(std::uint32_t shape)> cb);
  void setStateCallback(std::function<void(CefBrowserSessionState)> cb);
  void setPermissionRequestCallback(std::function<void(CefBrowserPermissionRequest)> cb);
  void setContextMenuRequestCallback(std::function<void(CefBrowserContextMenuRequest)> cb);
  void setPopupCreatedCallback(std::function<void(CefBrowserPopupRequest)> cb);
  void setClosedCallback(std::function<void()> cb);
  void setMediaStateCallback(std::function<void(const CefBrowserMediaState&)> cb);

private:
  friend class CefService;
  CefBrowserSession(CefService& service, std::string id);
  void attachGraphicsDevice(GraphicsDevice& graphics);
  void prepareForGraphicsDeviceRebuild();
  void resumeAfterGraphicsDeviceRebuild(GraphicsDevice& graphics);
  void beginShutdown();
  [[nodiscard]] bool browserClosed() const noexcept;
  void finishShutdown();

public:
  // Opaque CEF-owning state. Public only so the .cpp-local callback classes can
  // name it without exposing any CEF headers to shell code.
  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
  std::string m_id;
};
