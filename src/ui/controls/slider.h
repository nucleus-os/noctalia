#pragma once

#include "ui/controls/flex.h"
#include "ui/style.h"

#include <cstdint>
#include <functional>

class InputArea;
class RectNode;

// Vertical sliders grow upward: the minimum sits at the bottom edge.
enum class SliderOrientation : std::uint8_t { Horizontal, Vertical };

class Slider : public Flex {
public:
  Slider();

  void setOrientation(SliderOrientation orientation);
  void setRange(double minValue, double maxValue);
  void setStep(double step);
  void setValue(double value);
  void setEnabled(bool enabled);
  void setTrackHeight(float height);
  void setThumbSize(float size);
  void setControlHeight(float height);
  void setWheelAdjustEnabled(bool enabled);
  void setOnValueChanged(std::function<void(double)> callback);
  void setOnDragEnd(std::function<void()> callback);

  [[nodiscard]] double value() const noexcept { return m_value; }
  [[nodiscard]] double minValue() const noexcept { return m_min; }
  [[nodiscard]] double maxValue() const noexcept { return m_max; }
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] bool wheelAdjustEnabled() const noexcept { return m_wheelAdjustEnabled; }
  [[nodiscard]] SliderOrientation orientation() const noexcept { return m_orientation; }
  [[nodiscard]] bool dragging() const noexcept;

private:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void updateFromLocal(float x, float y);
  void updateGeometry();
  void applyVisualState();
  [[nodiscard]] float normalizedValue() const noexcept;
  [[nodiscard]] double snapped(double value) const noexcept;

  RectNode* m_track = nullptr;
  RectNode* m_fill = nullptr;
  RectNode* m_thumb = nullptr;
  InputArea* m_inputArea = nullptr;

  std::function<void(double)> m_onValueChanged;
  std::function<void()> m_onDragEnd;

  double m_min = 0.0;
  double m_max = 100.0;
  double m_step = 1.0;
  double m_value = 50.0;
  bool m_enabled = true;
  bool m_wheelAdjustEnabled = false;
  SliderOrientation m_orientation = SliderOrientation::Horizontal;
  float m_trackHeight = Style::sliderTrackHeight;
  float m_thumbSizePx = Style::sliderThumbSize;
  float m_controlHeightPx = Style::controlHeight;
};
