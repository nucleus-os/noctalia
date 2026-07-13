#pragma once

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"

class EffectNode : public Node {
public:
  EffectNode() : Node(NodeType::Effect) {}

  [[nodiscard]] const EffectStyle& style() const noexcept { return m_style; }

  void setEffectType(EffectType type) {
    if (m_style.type == type) {
      return;
    }
    m_style.type = type;
    markPaintDirty();
  }

  void setTime(float time) {
    m_style.time = time;
    markPaintDirty();
  }

  void setRadius(float radius) {
    if (m_style.radius == radius) {
      return;
    }
    m_style.radius = radius;
    markPaintDirty();
  }

  void setBgColor(const Color& color) {
    if (m_style.bgColor == color) {
      return;
    }
    m_style.bgColor = color;
    markPaintDirty();
  }

  void setSky(const Color& top, const Color& bottom) {
    if (m_style.skyTop == top && m_style.skyBottom == bottom) {
      return;
    }
    m_style.skyTop = top;
    m_style.skyBottom = bottom;
    markPaintDirty();
  }

  void setNight(bool night) {
    if (m_style.night == night) {
      return;
    }
    m_style.night = night;
    markPaintDirty();
  }

  void setCloudAmount(float amount) {
    if (m_style.cloudAmount == amount) {
      return;
    }
    m_style.cloudAmount = amount;
    markPaintDirty();
  }

  void setIntensity(float intensity) {
    if (m_style.intensity == intensity) {
      return;
    }
    m_style.intensity = intensity;
    markPaintDirty();
  }

private:
  EffectStyle m_style;
};
