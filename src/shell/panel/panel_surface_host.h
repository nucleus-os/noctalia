#pragma once

#include "ui/dialogs/layer_popup_host.h"

#include <optional>

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
  // Parent geometry for popups the panel opens itself (e.g. a CEF context menu): either the
  // shared layer surface (native panels) or this panel's own xdg_surface (toplevel-presented
  // panels). LayerPopupParentContext's xdgSurface field carries the latter case.
  [[nodiscard]] virtual std::optional<LayerPopupParentContext> popupParentContext() const = 0;
};
