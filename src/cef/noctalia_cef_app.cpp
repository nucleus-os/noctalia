#include "cef/noctalia_cef_app.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace {

constexpr std::string_view kAppleMusicTransparentThemeScript = R"JS(
(() => {
  const id = 'noctalia-apple-music-transparent-theme-v5';
  const install = () => {
    const root = document.documentElement;
    if (!root) return false;
    let style = document.getElementById(id);
    if (!style) {
      style = document.createElement('style');
      style.id = id;
      root.appendChild(style);
    }
    style.textContent = `
      :root, html, body {
        background-color: transparent !important;
        background-image: none !important;
      }
      nav.navigation {
        background: rgba(38, 38, 40, 0.28) !important;
        -webkit-backdrop-filter: saturate(2.2) blur(24px) !important;
        backdrop-filter: saturate(2.2) blur(24px) !important;
      }
      nav.navigation [data-testid="native-cta"] {
        display: none !important;
      }
      nav.navigation [data-testid="native-cta"] + .auth-button {
        border-top: 0.5px solid rgba(0, 0, 0, 0.1) !important;
        margin-bottom: 0 !important;
        padding-block: 12px !important;
      }
    `;
    return true;
  };
  if (!install()) {
    new MutationObserver((_, observer) => {
      if (install()) observer.disconnect();
    }).observe(document, {childList: true, subtree: true});
  }
})();
)JS";

void appendCommaSeparatedSwitchValue(
    const CefRefPtr<CefCommandLine>& commandLine, const char* switchName, const char* value
) {
  std::string existing = commandLine->GetSwitchValue(switchName).ToString();
  for (std::size_t start = 0; start <= existing.size();) {
    const std::size_t end = existing.find(',', start);
    const std::string_view entry(existing.data() + start, (end == std::string::npos ? existing.size() : end) - start);
    if (entry == value) {
      return;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  if (!existing.empty()) {
    existing.push_back(',');
  }
  existing.append(value);
  commandLine->AppendSwitchWithValue(switchName, existing);
}

} // namespace

bool prepareNoctaliaWidevineHint(
    const std::string& rootCachePath, const std::string& widevinePath, std::string& error
) {
  namespace fs = std::filesystem;
  std::error_code filesystemError;
  fs::path base = fs::path(widevinePath);
  if (!fs::is_regular_file(base / "manifest.json", filesystemError)) {
    filesystemError.clear();
    if (fs::is_regular_file(base / "libwidevinecdm.so", filesystemError)
        && base.filename() == "linux_x64" && base.parent_path().filename() == "_platform_specific") {
      base = base.parent_path().parent_path();
    }
  }
  filesystemError.clear();
  if (!fs::is_regular_file(base / "manifest.json", filesystemError)
      || !fs::is_regular_file(base / "_platform_specific/linux_x64/libwidevinecdm.so", filesystemError)) {
    error = "Widevine path does not contain a Linux x64 CDM and manifest: " + widevinePath;
    return false;
  }

  const fs::path hintDirectory = fs::path(rootCachePath) / "WidevineCdm";
  if (!fs::create_directories(hintDirectory, filesystemError) && filesystemError) {
    error = "failed to create Widevine hint directory: " + filesystemError.message();
    return false;
  }
  const fs::path hint = hintDirectory / "latest-component-updated-widevine-cdm";
  const fs::path temporary = hintDirectory / "latest-component-updated-widevine-cdm.tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      error = "failed to create Widevine hint file";
      return false;
    }
    output << "{\"Path\":" << std::quoted(base.string()) << "}\n";
    if (!output) {
      error = "failed to write Widevine hint file";
      return false;
    }
  }
  fs::rename(temporary, hint, filesystemError);
  if (filesystemError) {
    error = "failed to install Widevine hint file: " + filesystemError.message();
    return false;
  }
  return true;
}

void NoctaliaCefApp::OnBeforeCommandLineProcessing(const CefString& processType, CefRefPtr<CefCommandLine> cmd) {
  // Browser process only (subprocess types are non-empty).
  if (!processType.empty()) {
    return;
  }
  // Media/DRM playback should start without a user gesture (we drive it).
  cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
  // Chrome's Read Anything soft-navigation observer assumes a normal tab and
  // crashes in TabInterface::GetFromContents for CEF's windowless WebContents.
  // It is irrelevant to this embedded browser, so keep field trials from
  // enabling it in the browser process.
  appendCommaSeparatedSwitchValue(cmd, "disable-features", "ImmersiveReadAnything");
  appendCommaSeparatedSwitchValue(cmd, "enable-features", "OverlayScrollbar");
  // The accelerated OSR contract is Wayland + ANGLE Vulkan. ANGLE writes into
  // the exportable native pixmap consumed by Noctalia; Chromium's separate
  // native-Vulkan compositor is not part of this path.
  cmd->AppendSwitchWithValue("ozone-platform", "wayland");
  cmd->AppendSwitchWithValue("use-angle", "vulkan");
}

void NoctaliaCefApp::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> cmd) {
  // Forward the compositor-selected GPU explicitly so ANGLE, native Vulkan,
  // and the Wayland native-pixmap allocator all use Noctalia's device.
  if (const char* uuid = std::getenv("NOCTALIA_CEF_VULKAN_DEVICE_UUID");
      uuid != nullptr && uuid[0] != '\0') {
    cmd->AppendSwitchWithValue("cef-vulkan-device-uuid", uuid);
  }
  if (const char* renderNode = std::getenv("NOCTALIA_CEF_DRM_RENDER_NODE");
      renderNode != nullptr && renderNode[0] != '\0') {
    cmd->AppendSwitchWithValue("render-node-override", renderNode);
  }
}

void NoctaliaCefApp::OnScheduleMessagePumpWork(std::int64_t delayMs) {
  if (m_scheduleWork) {
    m_scheduleWork(delayMs);
  }
}

void NoctaliaCefApp::OnContextCreated(
    CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> /*context*/
) {
  if (!frame->IsMain()) {
    return;
  }
  const std::string url = frame->GetURL().ToString();
  if (url.starts_with("https://music.apple.com/")) {
    frame->ExecuteJavaScript(std::string(kAppleMusicTransparentThemeScript), frame->GetURL(), 0);
  }
}
