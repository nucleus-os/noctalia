#include "shell/bar/widgets/web_panel_launcher_widget.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>
#include <utility>

WebPanelLauncherWidget::WebPanelLauncherWidget(std::string panelId, std::string glyph, std::string tooltip)
    : m_panelId(std::move(panelId)), m_glyphName(std::move(glyph)), m_tooltip(std::move(tooltip)) {}

void WebPanelLauncherWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData&) { requestPanelToggle(m_panelId); });
  area->setTooltip(m_tooltip);
  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = m_glyphName,
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );
  setRoot(std::move(area));
}

void WebPanelLauncherWidget::doLayout(Renderer& renderer, float, float) {
  if (m_glyph == nullptr || root() == nullptr) {
    return;
  }
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  root()->setSize(m_glyph->width(), m_glyph->height());
}
