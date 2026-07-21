#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/niri_window_correlator.h"

#include <cassert>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>

using compositors::niri::NiriRuntime;
using compositors::niri::NiriWindowCorrelator;

namespace {

  // Constructing a NiriRuntime/NiriWindowCorrelator never touches a socket unless something
  // calls a request*/connect path, so this exercises the correlation logic in complete
  // isolation from a live niri instance -- the socket/reconnect/framing layer this correlator
  // sits on top of is already exercised in production by NiriWorkspaceBackend.
  nlohmann::json windowOpenedOrChanged(std::optional<std::string> appId, std::optional<long> pid, std::uint64_t id) {
    nlohmann::json window;
    window["id"] = id;
    window["app_id"] = appId.has_value() ? nlohmann::json(*appId) : nlohmann::json(nullptr);
    window["pid"] = pid.has_value() ? nlohmann::json(*pid) : nlohmann::json(nullptr);
    window["is_focused"] = false;
    window["is_floating"] = true;

    nlohmann::json event;
    event["window"] = window;
    return event;
  }

} // namespace

int main() {
  const std::string kAppId = "dev.noctalia.panel-open-abc123";
  const long kPid = 4242;

  // 1. Matching (app_id, pid) fires exactly once, with the right window id.
  {
    NiriRuntime runtime;
    int callCount = 0;
    std::uint64_t reportedId = 0;
    NiriWindowCorrelator correlator(runtime, kAppId, kPid, [&](std::uint64_t id) {
      ++callCount;
      reportedId = id;
    });

    assert(!correlator.matched());

    // A same-app_id window from a different process must not match.
    correlator.handleEvent("WindowOpenedOrChanged", windowOpenedOrChanged(kAppId, kPid + 1, 7));
    assert(!correlator.matched());
    assert(callCount == 0);

    // An unrelated event key must be ignored entirely.
    nlohmann::json unrelated;
    unrelated["id"] = 99;
    correlator.handleEvent("WindowClosed", unrelated);
    assert(!correlator.matched());

    // A window with no pid (e.g. via xdg-desktop-portal-gnome) must never match, even with the
    // right app_id -- there is nothing to correlate against.
    correlator.handleEvent("WindowOpenedOrChanged", windowOpenedOrChanged(kAppId, std::nullopt, 8));
    assert(!correlator.matched());

    // The real match.
    correlator.handleEvent("WindowOpenedOrChanged", windowOpenedOrChanged(kAppId, kPid, 1234));
    assert(correlator.matched());
    assert(callCount == 1);
    assert(reportedId == 1234);

    // A second, otherwise-matching event after the first match must not fire the callback
    // again -- WindowOpenedOrChanged also fires for property changes on an already-open window,
    // not just the initial map.
    correlator.handleEvent("WindowOpenedOrChanged", windowOpenedOrChanged(kAppId, kPid, 1234));
    assert(callCount == 1);
  }

  // 2. A concurrently open panel with a different nonce'd app_id never matches.
  {
    NiriRuntime runtime;
    int callCount = 0;
    NiriWindowCorrelator correlator(runtime, "dev.noctalia.panel-open-other999", kPid,
                                     [&](std::uint64_t) { ++callCount; });

    correlator.handleEvent("WindowOpenedOrChanged", windowOpenedOrChanged(kAppId, kPid, 1234));
    assert(!correlator.matched());
    assert(callCount == 0);
  }

  // 3. Malformed/partial payloads must not crash and must not match.
  {
    NiriRuntime runtime;
    NiriWindowCorrelator correlator(runtime, kAppId, kPid, [](std::uint64_t) { assert(false); });

    nlohmann::json noWindowKey;
    noWindowKey["notWindow"] = 1;
    correlator.handleEvent("WindowOpenedOrChanged", noWindowKey);
    assert(!correlator.matched());

    nlohmann::json windowNotObject;
    windowNotObject["window"] = 5;
    correlator.handleEvent("WindowOpenedOrChanged", windowNotObject);
    assert(!correlator.matched());

    nlohmann::json missingId;
    missingId["window"] = {{"app_id", kAppId}, {"pid", kPid}};
    correlator.handleEvent("WindowOpenedOrChanged", missingId);
    assert(!correlator.matched());
  }

  return 0;
}
