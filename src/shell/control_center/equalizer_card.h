#pragma once

#include "system/equalizer_service.h"
#include "ui/controls/flex.h"

#include <chrono>
#include <cstddef>
#include <vector>

class Button;
class Label;
class Renderer;
class Select;
class Slider;

// Live equalizer faders backed by EqualizerService (EasyEffects >= 8.2).
//
// Reading the band layout costs one socket round-trip per property, so the card
// never polls: it re-reads only when markStale() is called (panel open, effects
// profile switch) and otherwise just pushes fader movements out.
class EqualizerCard : public Flex {
public:
  EqualizerCard(EqualizerService* service, AudioEffectsProfileKind kind, float scale);

  // Re-reads the equalizer from EasyEffects on the next sync().
  void markStale() noexcept { m_stale = true; }
  void sync(Renderer& renderer);

  [[nodiscard]] bool dragging() const noexcept;

private:
  struct BandControls {
    Flex* column = nullptr;
    Slider* fader = nullptr;
    Label* gainLabel = nullptr;
    Label* frequencyLabel = nullptr;
  };

  void reload(Renderer& renderer);
  void rebuildBands(Renderer& renderer);
  void applyStatusText();
  void applyPreset(std::size_t index);
  void queueBandGain(int band, double gainDb);
  void flushPendingGain();

  EqualizerService* m_service = nullptr;
  AudioEffectsProfileKind m_kind = AudioEffectsProfileKind::Output;
  float m_scale = 1.0f;

  Flex* m_bandRow = nullptr;
  Label* m_statusLabel = nullptr;
  Select* m_presetSelect = nullptr;
  Button* m_resetButton = nullptr;
  std::vector<BandControls> m_bands;

  EqualizerService::State m_state;
  bool m_stale = true;
  bool m_built = false;
  bool m_syncing = false;
  // Drags emit at frame rate; coalesce them so we do not flood the socket.
  int m_pendingBand = -1;
  double m_pendingGainDb = 0.0;
  std::chrono::steady_clock::time_point m_lastCommitAt;
};
