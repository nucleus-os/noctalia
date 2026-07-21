#pragma once

#include "shell/bar/widget.h"

#include <string>

class Glyph;

class WebPanelLauncherWidget final : public Widget {
public:
  WebPanelLauncherWidget(std::string panelId, std::string glyph, std::string tooltip);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;

  std::string m_panelId;
  std::string m_glyphName;
  std::string m_tooltip;
  Glyph* m_glyph = nullptr;
};
