#pragma once

class InputArea;
class InputDispatcher;

// A panel's actual Wayland surface owner. PanelManager implements this for panels on the shared
// layer-shell surface; PanelManager::CefPanelToplevelHost implements it for panels in their own
// persistent xdg_toplevel. Panel::surfaceHost() lets a panel (e.g. WebPanel, which drives its
// own redraw/focus scheduling from CEF callbacks) reach whichever one actually owns it, instead
// of assuming there is exactly one panel surface system-wide via PanelManager::instance() — that
// assumption breaks once more than one toplevel-presented panel can be open at once.
class PanelSurfaceHost {
public:
  virtual ~PanelSurfaceHost() = default;

  virtual void requestUpdateOnly() = 0;
  virtual void requestLayout() = 0;
  virtual void requestRedraw() = 0;
  virtual void requestFrameTick() = 0;
  virtual void requestCallbackTick() = 0;
  virtual void focusArea(InputArea* area) = 0;
  [[nodiscard]] virtual InputDispatcher& inputDispatcher() = 0;
};
