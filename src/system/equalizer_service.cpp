#include "system/equalizer_service.h"

#include "system/easyeffects_ipc.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

  // EasyEffects addresses the first equalizer instance as "equalizer#0".
  constexpr const char* kPlugin = "equalizer";
  constexpr int kInstance = 0;
  // EasyEffects defines band0..band31 in its equalizer channel schema.
  constexpr int kMaxBands = 32;
  // EasyEffects band type 0 is "Off", 1 is "Bell". Changing numBands only ever
  // switches surplus bands off, so a preset must switch its own bands back on.
  constexpr double kBandTypeBell = 1.0;

  using PresetBand = EqualizerService::PresetBand;

  // All built-in curves share a 10-band ISO octave layout.
  constexpr std::array<PresetBand, 10> kFlat{
      {{31.5, 0.0},
       {63.0, 0.0},
       {125.0, 0.0},
       {250.0, 0.0},
       {500.0, 0.0},
       {1000.0, 0.0},
       {2000.0, 0.0},
       {4000.0, 0.0},
       {8000.0, 0.0},
       {16000.0, 0.0}}
  };

  constexpr std::array<PresetBand, 10> kBalanced{
      {{31.5, 2.0},
       {63.0, 1.5},
       {125.0, 1.0},
       {250.0, 0.0},
       {500.0, -0.5},
       {1000.0, 0.0},
       {2000.0, 0.5},
       {4000.0, 1.0},
       {8000.0, 2.0},
       {16000.0, 2.5}}
  };

  constexpr std::array<PresetBand, 10> kBassBoost{
      {{31.5, 7.0},
       {63.0, 6.0},
       {125.0, 4.5},
       {250.0, 2.0},
       {500.0, 0.0},
       {1000.0, 0.0},
       {2000.0, 0.0},
       {4000.0, 0.5},
       {8000.0, 1.0},
       {16000.0, 1.5}}
  };

  constexpr std::array<PresetBand, 10> kVocal{
      {{31.5, -3.0},
       {63.0, -2.0},
       {125.0, -1.0},
       {250.0, 0.5},
       {500.0, 2.0},
       {1000.0, 3.0},
       {2000.0, 3.0},
       {4000.0, 2.0},
       {8000.0, 0.5},
       {16000.0, -1.0}}
  };

  constexpr std::array<PresetBand, 10> kRock{
      {{31.5, 4.0},
       {63.0, 3.0},
       {125.0, 1.5},
       {250.0, -0.5},
       {500.0, -1.5},
       {1000.0, 0.0},
       {2000.0, 1.5},
       {4000.0, 3.0},
       {8000.0, 3.5},
       {16000.0, 3.0}}
  };

  constexpr std::array<PresetBand, 10> kElectronic{
      {{31.5, 5.0},
       {63.0, 4.5},
       {125.0, 2.0},
       {250.0, 0.0},
       {500.0, -1.5},
       {1000.0, 0.5},
       {2000.0, 1.0},
       {4000.0, 2.0},
       {8000.0, 3.5},
       {16000.0, 4.0}}
  };

  // Pulls in the extremes so quiet listening stays intelligible.
  constexpr std::array<PresetBand, 10> kLateNight{
      {{31.5, -2.0},
       {63.0, -1.0},
       {125.0, 0.5},
       {250.0, 2.0},
       {500.0, 2.5},
       {1000.0, 2.5},
       {2000.0, 2.0},
       {4000.0, 1.0},
       {8000.0, -0.5},
       {16000.0, -2.0}}
  };

  constexpr std::array<EqualizerService::Preset, 7> kPresets{
      {{"flat", "control-center.audio.equalizer-preset.flat", kFlat},
       {"balanced", "control-center.audio.equalizer-preset.balanced", kBalanced},
       {"bass-boost", "control-center.audio.equalizer-preset.bass-boost", kBassBoost},
       {"vocal", "control-center.audio.equalizer-preset.vocal", kVocal},
       {"rock", "control-center.audio.equalizer-preset.rock", kRock},
       {"electronic", "control-center.audio.equalizer-preset.electronic", kElectronic},
       {"late-night", "control-center.audio.equalizer-preset.late-night", kLateNight}}
  };

  const char* pipelineName(AudioEffectsProfileKind kind) {
    return kind == AudioEffectsProfileKind::Input ? "input" : "output";
  }

  // A reply that begins with "error" is one of EasyEffects' error tokens
  // (error_plugin_not_found / error_property_not_found / ...), not a value.
  bool isErrorReply(std::string_view reply) {
    return reply.starts_with("error");
  }

  std::optional<double> parseDouble(const std::string& s) {
    if (s.empty()) {
      return std::nullopt;
    }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') {
      return std::nullopt; // not fully numeric
    }
    return v;
  }

  std::string getPropertyCommand(AudioEffectsProfileKind kind, std::string_view channel, std::string_view property) {
    if (channel.empty()) {
      return std::format("get_property:{}:{}:{}:{}", pipelineName(kind), kPlugin, kInstance, property);
    }
    return std::format("get_property:{}:{}:{}:{}:{}", pipelineName(kind), kPlugin, kInstance, channel, property);
  }

  std::string
  setPropertyCommand(AudioEffectsProfileKind kind, std::string_view channel, std::string_view property, double value) {
    // {:.6g} keeps the decimal point locale-independent and avoids scientific
    // notation for the equalizer's [-36, 36] dB range.
    return std::format(
        "set_property:{}:{}:{}:{}:{}:{:.6g}", pipelineName(kind), kPlugin, kInstance, channel, property, value
    );
  }

  // Raw property read: nullopt when EasyEffects gave no reply (unreachable),
  // otherwise the trimmed reply (which may be an error token or a value).
  std::optional<std::string>
  getProperty(AudioEffectsProfileKind kind, std::string_view channel, std::string_view property) {
    auto reply = easyeffects::ipc::exchange(getPropertyCommand(kind, channel, property), true);
    if (!reply.has_value()) {
      return std::nullopt;
    }
    return StringUtils::trim(*reply);
  }

} // namespace

EqualizerService::Status EqualizerService::status(AudioEffectsProfileKind kind) {
  if (!easyeffects::ipc::running()) {
    return Status::NotRunning;
  }
  const auto numBands = getProperty(kind, "", "numBands");
  if (!numBands.has_value()) {
    return Status::NotRunning;
  }
  if (isErrorReply(*numBands)) {
    return Status::NoEqualizer;
  }
  // Probe a channel band property: on EasyEffects < 8.2 the set/get command has
  // no channel field, so this returns error_property_not_found.
  const auto band0 = getProperty(kind, "left", "band0Gain");
  if (!band0.has_value()) {
    return Status::NotRunning;
  }
  if (isErrorReply(*band0)) {
    return Status::Unsupported;
  }
  return Status::Ok;
}

EqualizerService::State EqualizerService::read(AudioEffectsProfileKind kind) {
  State state;

  if (!easyeffects::ipc::running()) {
    return state; // NotRunning
  }

  const auto numBandsReply = getProperty(kind, "", "numBands");
  if (!numBandsReply.has_value()) {
    return state; // NotRunning
  }
  if (isErrorReply(*numBandsReply)) {
    state.status = Status::NoEqualizer;
    return state;
  }

  const auto numBands = parseDouble(*numBandsReply);
  const int count = numBands.has_value() ? std::clamp(static_cast<int>(*numBands), 0, kMaxBands) : 0;

  if (const auto split = getProperty(kind, "", "splitChannels"); split.has_value()) {
    state.splitChannels = (*split == "true" || *split == "1");
  }

  state.bands.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    const auto gainReply = getProperty(kind, "left", std::format("band{}Gain", i));
    if (!gainReply.has_value()) {
      state.status = Status::NotRunning;
      state.bands.clear();
      return state;
    }
    const auto gain = isErrorReply(*gainReply) ? std::nullopt : parseDouble(*gainReply);
    if (!gain.has_value()) {
      // The first band failing to resolve means this EasyEffects lacks per-band
      // channel control (< 8.2); report Unsupported rather than a partial read.
      state.status = Status::Unsupported;
      state.bands.clear();
      return state;
    }
    double frequency = 0.0;
    if (const auto freqReply = getProperty(kind, "left", std::format("band{}Frequency", i));
        freqReply.has_value() && !isErrorReply(*freqReply)) {
      frequency = parseDouble(*freqReply).value_or(0.0);
    }
    state.bands.push_back(Band{.index = i, .frequencyHz = frequency, .gainDb = *gain});
  }

  state.status = Status::Ok;
  return state;
}

bool EqualizerService::setBandGain(AudioEffectsProfileKind kind, int band, double gainDb) {
  if (band < 0 || band >= kMaxBands) {
    return false;
  }
  const double clamped = std::clamp(gainDb, kMinGainDb, kMaxGainDb);
  const std::string property = std::format("band{}Gain", band);

  // Write both channels so a single fader works whether or not split-channels is
  // enabled. The left write determines the reported result; right is applied
  // best-effort to keep the channels coherent.
  const bool ok = easyeffects::ipc::send(setPropertyCommand(kind, "left", property, clamped));
  easyeffects::ipc::send(setPropertyCommand(kind, "right", property, clamped));
  return ok;
}

std::span<const EqualizerService::Preset> EqualizerService::presets() noexcept { return kPresets; }

bool EqualizerService::applyPreset(AudioEffectsProfileKind kind, const Preset& preset) {
  const auto count = static_cast<int>(std::min<std::size_t>(preset.bands.size(), kMaxBands));
  if (count <= 0) {
    return false;
  }

  std::vector<std::string> commands;
  commands.reserve(static_cast<std::size_t>(count) * 6 + 1);

  // Band count first: EasyEffects switches surplus bands off as a side effect,
  // so the per-band writes below must come after it.
  commands.push_back(
      std::format("set_property:{}:{}:{}:numBands:{}", pipelineName(kind), kPlugin, kInstance, count)
  );

  for (int i = 0; i < count; ++i) {
    const auto& band = preset.bands[static_cast<std::size_t>(i)];
    const double gain = std::clamp(band.gainDb, kMinGainDb, kMaxGainDb);
    for (const std::string_view channel : {"left", "right"}) {
      commands.push_back(setPropertyCommand(kind, channel, std::format("band{}Type", i), kBandTypeBell));
      commands.push_back(setPropertyCommand(kind, channel, std::format("band{}Frequency", i), band.frequencyHz));
      commands.push_back(setPropertyCommand(kind, channel, std::format("band{}Gain", i), gain));
    }
  }

  return easyeffects::ipc::sendAll(commands);
}
