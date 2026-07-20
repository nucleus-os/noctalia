#pragma once

#include "system/easyeffects_service.h" // AudioEffectsProfileKind

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Live read/write of the EasyEffects equalizer plugin through its socket
// property API (set_property / get_property). Setting a band gain applies to
// the running audio pipeline in real time - the same mechanism EasyEffects' own
// UI uses - so it is suitable for driving under a dragging fader. Whole-preset
// management stays in EasyEffectsService; this class only touches live plugin
// state.
//
// Requirements and degradation:
//   * Per-band channel control requires EasyEffects >= 8.2. On 8.1.x the
//     set_property command has no channel field and band gains are unreachable;
//     status() reports Unsupported and setBandGain() is a no-op.
//   * The target is the first equalizer instance ("equalizer#0") on the given
//     pipeline. If the active preset has no equalizer, status() reports
//     NoEqualizer.
//
// All calls target instance 0 and are cheap local-socket round-trips. read() is
// for populating UI (open panel / refresh); setBandGain() is for live edits.
class EqualizerService {
public:
  enum class Status {
    NotRunning,  // EasyEffects is not reachable
    NoEqualizer, // running, but the active pipeline has no equalizer plugin
    Unsupported, // running with an equalizer, but the EE version lacks per-band channel control (< 8.2)
    Ok,          // per-band control is available
  };

  struct Band {
    int index = 0;
    double frequencyHz = 0.0;
    double gainDb = 0.0;
  };

  struct State {
    Status status = Status::NotRunning;
    bool splitChannels = false;
    std::vector<Band> bands; // left-channel view (single fader per band)
  };

  // One band of a named preset curve.
  struct PresetBand {
    double frequencyHz = 0.0;
    double gainDb = 0.0;
  };

  // A named EQ curve. Applying one rewrites the whole band layout (count,
  // frequencies, gains), so presets are self-contained and do not depend on how
  // the equalizer happened to be configured before.
  struct Preset {
    std::string_view id;       // stable identifier
    std::string_view labelKey; // i18n key for the display name
    std::span<const PresetBand> bands;
  };

  // Built-in curves, in display order. The first entry is always flat.
  [[nodiscard]] static std::span<const Preset> presets() noexcept;

  // EasyEffects' equalizer band gain range, in dB.
  static constexpr double kMinGainDb = -36.0;
  static constexpr double kMaxGainDb = 36.0;

  // Snapshot the current equalizer on the given pipeline for the UI.
  [[nodiscard]] State read(AudioEffectsProfileKind kind);

  // Live-apply a band's gain (dB, clamped to [kMinGainDb, kMaxGainDb]) on both
  // channels so it works whether or not split-channels is enabled. Fire and
  // forget: EasyEffects applies it immediately. Returns false if the command
  // could not be delivered (EasyEffects unreachable).
  bool setBandGain(AudioEffectsProfileKind kind, int band, double gainDb);

  // Rewrite the equalizer to a preset curve: band count, then each band's type,
  // frequency and gain on both channels. Sent as one batch, so EasyEffects
  // applies the whole curve in a single pass. Returns false if the batch could
  // not be delivered.
  //
  // Note this replaces any per-band frequencies the user set in EasyEffects.
  bool applyPreset(AudioEffectsProfileKind kind, const Preset& preset);

  // Capability probe for the current pipeline state (running / has-equalizer /
  // version supports channel control). Prefer read() when you also need bands.
  [[nodiscard]] Status status(AudioEffectsProfileKind kind);
};
