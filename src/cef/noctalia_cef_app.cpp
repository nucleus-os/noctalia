#include "cef/noctalia_cef_app.h"

#include <cstdlib>

void NoctaliaCefApp::OnBeforeCommandLineProcessing(const CefString& processType, CefRefPtr<CefCommandLine> cmd) {
  // Browser process only (subprocess types are non-empty).
  if (!processType.empty()) {
    return;
  }
  // Media/DRM playback should start without a user gesture (we drive it).
  cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
  // GPU compositing remains enabled: production OSR requires shared textures.

  // Optional explicit Widevine CDM directory (dir containing libwidevinecdm.so).
  // With a codec-enabled Chrome-runtime build the component updater usually
  // finds one, but this lets us pin Chrome's CDM when needed.
  if (const char* widevine = std::getenv("NOCTALIA_CEF_WIDEVINE"); widevine != nullptr && widevine[0] != '\0') {
    cmd->AppendSwitchWithValue("widevine-cdm-path", widevine);
  }
}

void NoctaliaCefApp::OnScheduleMessagePumpWork(std::int64_t delayMs) {
  if (m_scheduleWork) {
    m_scheduleWork(delayMs);
  }
}
