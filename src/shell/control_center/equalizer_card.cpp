#include "shell/control_center/equalizer_card.h"

#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "shell/control_center/tab.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr auto kGainCommitInterval = std::chrono::milliseconds(16);
  constexpr float kFaderHeight = 120.0f;
  constexpr float kFaderColumnWidth = 44.0f;

  std::string formatGain(double gainDb) {
    // Keep the sign visible so a flat band reads differently from a cut.
    const double rounded = std::round(gainDb * 10.0) / 10.0;
    return std::format("{:+.1f}", rounded == 0.0 ? 0.0 : rounded);
  }

  std::string formatFrequency(double hz) {
    if (hz <= 0.0) {
      return "-";
    }
    if (hz >= 1000.0) {
      const double khz = hz / 1000.0;
      return khz >= 10.0 ? std::format("{:.0f}k", khz) : std::format("{:.1f}k", khz);
    }
    return std::format("{:.0f}", hz);
  }

  std::vector<std::string> presetLabels() {
    std::vector<std::string> labels;
    const auto presets = EqualizerService::presets();
    labels.reserve(presets.size());
    for (const auto& preset : presets) {
      labels.push_back(i18n::tr(preset.labelKey));
    }
    return labels;
  }

  std::string_view statusKey(EqualizerService::Status status) {
    switch (status) {
    case EqualizerService::Status::NotRunning:
      return "control-center.audio.equalizer-not-running";
    case EqualizerService::Status::NoEqualizer:
      return "control-center.audio.equalizer-missing";
    case EqualizerService::Status::Unsupported:
      return "control-center.audio.equalizer-unsupported";
    case EqualizerService::Status::Ok:
      break;
    }
    return {};
  }

} // namespace

EqualizerCard::EqualizerCard(EqualizerService* service, AudioEffectsProfileKind kind, float scale)
    : m_service(service), m_kind(kind), m_scale(scale) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm * scale);
  control_center::applySectionCardStyle(*this, scale);

  addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::SpaceBetween,
              .gap = Style::spaceSm * scale,
          },
          ui::label({
              .text = i18n::tr("control-center.audio.equalizer"),
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          }),
          ui::select({
              .out = &m_presetSelect,
              .options = presetLabels(),
              .placeholder = i18n::tr("control-center.audio.equalizer-choose-preset"),
              .fontSize = Style::fontSizeCaption * scale,
              .enabled = false,
              .flexGrow = 1.0f,
              .onSelectionChanged =
                  [this](std::size_t index, std::string_view /*label*/) { applyPreset(index); },
          }),
          ui::button({
              .out = &m_resetButton,
              .text = i18n::tr("control-center.audio.equalizer-reset"),
              .fontSize = Style::fontSizeCaption * scale,
              .enabled = false,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick =
                  [this]() {
                    for (std::size_t i = 0; i < m_bands.size(); ++i) {
                      if (m_bands[i].fader != nullptr) {
                        m_bands[i].fader->setValue(0.0);
                      }
                      if (m_service != nullptr) {
                        m_service->setBandGain(m_kind, static_cast<int>(i), 0.0);
                      }
                    }
                  },
          })
      )
  );

  addChild(ui::label({
      .out = &m_statusLabel,
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 2,
      .visible = false,
  }));

  addChild(ui::row({
      .out = &m_bandRow,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceXs * scale,
      .visible = false,
  }));
}

bool EqualizerCard::dragging() const noexcept {
  return std::ranges::any_of(m_bands, [](const BandControls& band) {
    return band.fader != nullptr && band.fader->dragging();
  });
}

void EqualizerCard::sync(Renderer& renderer) {
  // Never re-read mid-drag: the round-trips would fight the fader being held.
  if (m_stale && !dragging()) {
    reload(renderer);
  }

  if (m_pendingBand >= 0 && std::chrono::steady_clock::now() - m_lastCommitAt >= kGainCommitInterval) {
    flushPendingGain();
  }
}

void EqualizerCard::reload(Renderer& renderer) {
  m_stale = false;

  EqualizerService::State next =
      m_service != nullptr ? m_service->read(m_kind) : EqualizerService::State{};

  // The first pass always builds: an empty read matches the default-constructed
  // state, so the status message would otherwise never appear.
  const bool layoutChanged =
      !m_built || next.bands.size() != m_state.bands.size() || next.status != m_state.status;
  m_state = std::move(next);

  if (layoutChanged) {
    rebuildBands(renderer);
    m_built = true;
  }

  m_syncing = true;
  for (std::size_t i = 0; i < m_bands.size() && i < m_state.bands.size(); ++i) {
    const auto& band = m_state.bands[i];
    if (m_bands[i].fader != nullptr) {
      m_bands[i].fader->setValue(band.gainDb);
    }
    if (m_bands[i].gainLabel != nullptr) {
      m_bands[i].gainLabel->setText(formatGain(band.gainDb));
    }
    if (m_bands[i].frequencyLabel != nullptr) {
      m_bands[i].frequencyLabel->setText(formatFrequency(band.frequencyHz));
    }
  }
  m_syncing = false;
}

void EqualizerCard::rebuildBands(Renderer& renderer) {
  uiAssertNotRendering("EqualizerCard::rebuildBands");
  if (m_bandRow == nullptr) {
    return;
  }

  while (!m_bandRow->children().empty()) {
    m_bandRow->removeChild(m_bandRow->children().front().get());
  }
  m_bands.clear();

  applyStatusText();

  const bool hasBands = m_state.status == EqualizerService::Status::Ok && !m_state.bands.empty();
  m_bandRow->setVisible(hasBands);
  if (m_resetButton != nullptr) {
    m_resetButton->setEnabled(hasBands);
  }
  if (m_presetSelect != nullptr) {
    // Applying a preset writes to the equalizer plugin, so it needs one present.
    m_presetSelect->setEnabled(m_state.status == EqualizerService::Status::Ok);
  }
  if (!hasBands) {
    m_bandRow->layout(renderer);
    return;
  }

  m_bands.reserve(m_state.bands.size());
  for (const auto& band : m_state.bands) {
    BandControls controls;
    const int index = band.index;
    m_bandRow->addChild(
        ui::column(
            {
                .out = &controls.column,
                .align = FlexAlign::Center,
                .gap = Style::spaceXs * m_scale,
                .width = kFaderColumnWidth * m_scale,
            },
            ui::label({
                .out = &controls.gainLabel,
                .text = formatGain(band.gainDb),
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxLines = 1,
            }),
            ui::slider({
                .out = &controls.fader,
                .orientation = SliderOrientation::Vertical,
                .minValue = EqualizerService::kMinGainDb,
                .maxValue = EqualizerService::kMaxGainDb,
                .step = 0.5,
                .value = band.gainDb,
                .wheelAdjustEnabled = true,
                .height = kFaderHeight * m_scale,
                .onValueChanged =
                    [this, index](double value) {
                      if (m_syncing) {
                        return;
                      }
                      queueBandGain(index, value);
                    },
                .onDragEnd = [this]() { flushPendingGain(); },
            }),
            ui::label({
                .out = &controls.frequencyLabel,
                .text = formatFrequency(band.frequencyHz),
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
            })
        )
    );
    m_bands.push_back(controls);
  }

  m_bandRow->layout(renderer);
}

void EqualizerCard::applyStatusText() {
  if (m_statusLabel == nullptr) {
    return;
  }
  const std::string_view key = statusKey(m_state.status);
  const bool show = !key.empty();
  m_statusLabel->setVisible(show);
  if (show) {
    m_statusLabel->setText(i18n::tr(key));
  }
}

void EqualizerCard::applyPreset(std::size_t index) {
  const auto presets = EqualizerService::presets();
  if (m_service == nullptr || index >= presets.size()) {
    return;
  }
  // Drop any in-flight fader write: the preset rewrites every band anyway.
  m_pendingBand = -1;
  if (m_service->applyPreset(m_kind, presets[index])) {
    markStale();
  }
}

void EqualizerCard::queueBandGain(int band, double gainDb) {
  if (band < 0 || band >= static_cast<int>(m_bands.size())) {
    return;
  }
  // A hand-edited curve is no longer the selected preset.
  if (m_presetSelect != nullptr) {
    m_presetSelect->clearSelection();
  }
  if (m_bands[static_cast<std::size_t>(band)].gainLabel != nullptr) {
    m_bands[static_cast<std::size_t>(band)].gainLabel->setText(formatGain(gainDb));
  }
  m_pendingBand = band;
  m_pendingGainDb = gainDb;

  if (std::chrono::steady_clock::now() - m_lastCommitAt >= kGainCommitInterval) {
    flushPendingGain();
  }
}

void EqualizerCard::flushPendingGain() {
  if (m_pendingBand < 0) {
    return;
  }
  const int band = m_pendingBand;
  const double gain = m_pendingGainDb;
  m_pendingBand = -1;
  m_lastCommitAt = std::chrono::steady_clock::now();

  if (m_service == nullptr) {
    return;
  }
  m_service->setBandGain(m_kind, band, gain);
  if (band < static_cast<int>(m_state.bands.size())) {
    m_state.bands[static_cast<std::size_t>(band)].gainDb = gain;
  }
}
