#include "compositors/niri/niri_window_correlator.h"

#include <utility>

namespace compositors::niri {

  NiriWindowCorrelator::NiriWindowCorrelator(NiriRuntime& runtime, std::string expectedAppId, long expectedPid,
                                              std::function<void(std::uint64_t)> onMatched)
      : NiriEventHandler(runtime), m_expectedAppId(std::move(expectedAppId)), m_expectedPid(expectedPid),
        m_onMatched(std::move(onMatched)) {}

  void NiriWindowCorrelator::handleEvent(std::string_view key, const nlohmann::json& value) {
    if (m_matched || key != "WindowOpenedOrChanged") {
      return;
    }

    const auto windowIt = value.find("window");
    if (windowIt == value.end() || !windowIt->is_object()) {
      return;
    }
    const auto& window = *windowIt;

    const auto appIdIt = window.find("app_id");
    if (appIdIt == window.end() || !appIdIt->is_string() || appIdIt->get<std::string>() != m_expectedAppId) {
      return;
    }

    // pid is `Option<i32>` on the wire (null when niri doesn't know it, e.g. windows opened via
    // xdg-desktop-portal-gnome) — a null pid can never match a real caller pid, so this also
    // correctly rejects those without a separate null check.
    const auto pidIt = window.find("pid");
    if (pidIt == window.end() || !pidIt->is_number_integer() || pidIt->get<long>() != m_expectedPid) {
      return;
    }

    const auto idIt = window.find("id");
    if (idIt == window.end() || !idIt->is_number_unsigned()) {
      return;
    }

    m_matched = true;
    if (m_onMatched) {
      m_onMatched(idIt->get<std::uint64_t>());
    }
  }

} // namespace compositors::niri
