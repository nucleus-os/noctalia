#include "ui/controls/slider.h"

#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/clamp.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  constexpr double kValueEpsilon = 0.0001;

  RoundedRectStyle solidStyle(const Color& fill, float radius) {
    return RoundedRectStyle{
        .fill = fill,
        .border = fill,
        .fillMode = FillMode::Solid,
        .radius = radius,
        .softness = 1.0f,
        .borderWidth = 0.0f,
    };
  }

  Color resolved(ColorRole role, float alpha = 1.0f) { return colorForRole(role, alpha); }

} // namespace

Slider::Slider() {
  auto track = std::make_unique<RectNode>();
  m_track = static_cast<RectNode*>(addChild(std::move(track)));

  auto fill = std::make_unique<RectNode>();
  m_fill = static_cast<RectNode*>(addChild(std::move(fill)));

  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnLeave([this]() {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (!m_enabled || data.button != BTN_LEFT) {
      return;
    }
    if (!data.pressed) {
      applyVisualState();
      markPaintDirty();
      if (m_onDragEnd) {
        m_onDragEnd();
      }
      return;
    }
    applyVisualState();
    updateFromLocal(data.localX, data.localY);
    markPaintDirty();
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (!m_enabled || m_inputArea == nullptr || !m_inputArea->pressed()) {
      return;
    }
    updateFromLocal(data.localX, data.localY);
  });
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!m_enabled || !m_wheelAdjustEnabled || data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    const auto lines = static_cast<double>(data.scrollSteps());
    if (lines == 0.0) {
      return false;
    }
    // Per-line step: use the slider's snap step, else 5% of range.
    const double step = m_step > 0.0 ? m_step : (m_max - m_min) * 0.05;
    if (step <= 0.0) {
      return false;
    }
    // Wayland convention: positive axisLines = scroll down. Scroll up should increase.
    setValue(m_value - lines * step);
    if (m_onDragEnd) {
      m_onDragEnd();
    }
    return true;
  });
  area->setFocusable(true);
  area->setOnFocusGain([this]() {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnFocusLoss([this]() {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    const double step = m_step > 0.0 ? m_step : (m_max - m_min) * 0.05;
    if (step <= 0.0) {
      return;
    }
    // Each orientation takes the arrows that point along its own axis.
    const KeybindAction decreaseAction =
        m_orientation == SliderOrientation::Vertical ? KeybindAction::Down : KeybindAction::Left;
    const KeybindAction increaseAction =
        m_orientation == SliderOrientation::Vertical ? KeybindAction::Up : KeybindAction::Right;
    if (KeybindMatcher::matches(decreaseAction, key.sym, key.modifiers)) {
      setValue(m_value - step);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeybindMatcher::matches(increaseAction, key.sym, key.modifiers)) {
      setValue(m_value + step);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isPageDown(key.sym)) {
      setValue(m_value - step * 10.0);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isPageUp(key.sym)) {
      setValue(m_value + step * 10.0);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isHome(key.sym)) {
      setValue(m_min);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isEnd(key.sym)) {
      setValue(m_max);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);

  applyVisualState();
}

void Slider::setOrientation(SliderOrientation orientation) {
  if (m_orientation == orientation) {
    return;
  }
  m_orientation = orientation;
  updateGeometry();
  markLayoutDirty();
}

void Slider::setRange(double minValue, double maxValue) {
  if (maxValue < minValue) {
    std::swap(minValue, maxValue);
  }
  if (m_min == minValue && m_max == maxValue) {
    return;
  }
  m_min = minValue;
  m_max = maxValue;
  const double next = snapped(m_value);
  const bool valueChanged = std::abs(next - m_value) >= kValueEpsilon;
  m_value = next;
  updateGeometry();
  markPaintDirty();
  if (valueChanged && m_onValueChanged) {
    m_onValueChanged(m_value);
  }
}

void Slider::setStep(double step) {
  m_step = std::max(step, 0.0);
  const double next = snapped(m_value);
  const bool valueChanged = std::abs(next - m_value) >= kValueEpsilon;
  m_value = next;
  updateGeometry();
  markPaintDirty();
  if (valueChanged && m_onValueChanged) {
    m_onValueChanged(m_value);
  }
}

void Slider::setValue(double value) {
  const double next = snapped(value);
  if (std::abs(next - m_value) < kValueEpsilon) {
    return;
  }
  m_value = next;
  updateGeometry();
  markPaintDirty();
  if (m_onValueChanged) {
    m_onValueChanged(m_value);
  }
}

void Slider::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  applyVisualState();
  markPaintDirty();
}

void Slider::setTrackHeight(float height) {
  m_trackHeight = std::max(1.0f, height);
  updateGeometry();
  markLayoutDirty();
}

void Slider::setThumbSize(float size) {
  m_thumbSizePx = std::max(1.0f, size);
  updateGeometry();
  markLayoutDirty();
}

void Slider::setControlHeight(float height) {
  m_controlHeightPx = std::max(1.0f, height);
  updateGeometry();
  markLayoutDirty();
}

void Slider::setWheelAdjustEnabled(bool enabled) { m_wheelAdjustEnabled = enabled; }

void Slider::setOnValueChanged(std::function<void(double)> callback) { m_onValueChanged = std::move(callback); }

void Slider::setOnDragEnd(std::function<void()> callback) { m_onDragEnd = std::move(callback); }

bool Slider::dragging() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Slider::doLayout(Renderer& /*renderer*/) {
  updateGeometry();
  applyVisualState();
}

LayoutSize Slider::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Slider::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void Slider::updateGeometry() {
  // The cross axis is sized to fit track/thumb; the main axis takes whatever the
  // parent layout gave us, falling back to the default length.
  const float thickness = std::max({m_thumbSizePx, m_trackHeight, m_controlHeightPx});

  if (m_orientation == SliderOrientation::Vertical) {
    const float heightPx = height() > 0.0f ? height() : Style::sliderDefaultWidth;
    setSize(thickness, heightPx);

    const float trackX = (thickness - m_trackHeight) * 0.5f;
    const float trackY = Style::sliderHorizontalPadding;
    const float trackH = std::max(0.0f, heightPx - Style::sliderHorizontalPadding * 2.0f);
    // Vertical sliders read bottom-up, so the maximum is at the small Y.
    const float thumbCenterY = trackY + (1.0f - normalizedValue()) * trackH;
    const float thumbX = (thickness - m_thumbSizePx) * 0.5f;

    m_track->setPosition(trackX, trackY);
    m_track->setFrameSize(m_trackHeight, trackH);

    m_fill->setPosition(trackX, thumbCenterY);
    m_fill->setFrameSize(m_trackHeight, std::max(0.0f, trackY + trackH - thumbCenterY));

    m_thumb->setPosition(
        thumbX, util::clampOrdered(thumbCenterY - m_thumbSizePx * 0.5f, trackY, trackY + trackH - m_thumbSizePx)
    );
    m_thumb->setFrameSize(m_thumbSizePx, m_thumbSizePx);

    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(thickness, heightPx);
    return;
  }

  const float widthPx = width() > 0.0f ? width() : Style::sliderDefaultWidth;
  setSize(widthPx, thickness);

  const float trackY = (thickness - m_trackHeight) * 0.5f;
  const float trackX = Style::sliderHorizontalPadding;
  const float trackW = std::max(0.0f, widthPx - Style::sliderHorizontalPadding * 2.0f);
  const float thumbX = trackX + normalizedValue() * trackW;
  const float thumbY = (thickness - m_thumbSizePx) * 0.5f;

  m_track->setPosition(trackX, trackY);
  m_track->setFrameSize(trackW, m_trackHeight);

  m_fill->setPosition(trackX, trackY);
  m_fill->setFrameSize(std::max(0.0f, thumbX - trackX), m_trackHeight);

  m_thumb->setPosition(
      util::clampOrdered(thumbX - m_thumbSizePx * 0.5f, trackX, trackX + trackW - m_thumbSizePx), thumbY
  );
  m_thumb->setFrameSize(m_thumbSizePx, m_thumbSizePx);

  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setFrameSize(widthPx, thickness);
}

void Slider::updateFromLocal(float x, float y) {
  const bool vertical = m_orientation == SliderOrientation::Vertical;
  const float extent = vertical ? (height() > 0.0f ? height() : Style::sliderDefaultWidth)
                                : (width() > 0.0f ? width() : Style::sliderDefaultWidth);
  const float trackStart = Style::sliderHorizontalPadding;
  const float trackLength = std::max(0.0f, extent - Style::sliderHorizontalPadding * 2.0f);
  if (trackLength <= 0.0f) {
    return;
  }
  const float local = vertical ? y : x;
  double t = static_cast<double>(std::clamp((local - trackStart) / trackLength, 0.0f, 1.0f));
  if (vertical) {
    t = 1.0 - t;
  }
  setValue(m_min + t * (m_max - m_min));
}

void Slider::applyVisualState() {
  const bool hovering = m_inputArea != nullptr && m_inputArea->hovered();
  const bool pressing = m_inputArea != nullptr && m_inputArea->pressed();
  const bool focused = m_inputArea != nullptr && m_inputArea->focused();

  Color trackColor = resolved(ColorRole::Outline);
  Color fillColor = resolved(ColorRole::Primary);
  Color thumbColor = resolved(ColorRole::OnPrimary);
  Color thumbBorder = resolved(ColorRole::Outline);

  m_thumb->setVisible(m_enabled);

  if (!m_enabled) {
    trackColor = resolved(ColorRole::Outline, Style::disabledOutlineAlpha);
    fillColor = resolved(ColorRole::Primary, 0.5f);
  } else if (pressing) {
    fillColor = resolved(ColorRole::Primary);
  } else if (focused) {
    thumbBorder = resolveColorSpec(focusRingColorSpec());
  } else if (hovering) {
    thumbBorder = resolved(ColorRole::Hover);
  }

  auto trackStyle = solidStyle(trackColor, m_trackHeight * 0.5f);
  m_track->setStyle(trackStyle);

  auto fillStyle = solidStyle(fillColor, m_trackHeight * 0.5f);
  m_fill->setStyle(fillStyle);

  auto thumbStyle = solidStyle(thumbColor, m_thumbSizePx * 0.5f);
  thumbStyle.border = thumbBorder;
  thumbStyle.borderWidth = focused ? Style::focusRingWidth : Style::borderWidth;
  m_thumb->setStyle(thumbStyle);
}

float Slider::normalizedValue() const noexcept {
  if (m_max <= m_min) {
    return 0.0f;
  }
  return static_cast<float>(std::clamp((m_value - m_min) / (m_max - m_min), 0.0, 1.0));
}

double Slider::snapped(double value) const noexcept {
  const double clamped = std::clamp(value, m_min, m_max);
  if (m_step <= 0.0 || m_max <= m_min) {
    return clamped;
  }

  const double steps = std::round((clamped - m_min) / m_step);
  return std::clamp(m_min + steps * m_step, m_min, m_max);
}
