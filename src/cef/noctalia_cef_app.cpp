#include "cef/noctalia_cef_app.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace {

constexpr std::string_view kAppleMusicTransparentThemeScript = R"JS(
(() => {
  const id = 'noctalia-apple-music-transparent-theme-v4';
  const navigationSelector = 'nav.navigation[data-testid="navigation"], nav.navigation';
  const playerSelector = '.player-bar';
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
        /*
         * Apple assigns navigation, its header, and sticky content chrome the
         * same --z-web-chrome level. Later content can consequently paint over
         * navigation's backdrop. Raise navigation by one local chrome level
         * while leaving contextual menus and modals above it. Keep the filter
         * on the navigation itself: moving it to a negative-z pseudo-element
         * creates a different backdrop root and can leave independently
         * composited text outside the filtered result.
         */
        z-index: calc(var(--z-web-chrome, 9901) + 1) !important;
        background: rgba(38, 38, 40, 0.28) !important;
        -webkit-backdrop-filter: saturate(2.2) blur(24px) !important;
        backdrop-filter: saturate(2.2) blur(24px) !important;
      }
      .player-bar {
        /*
         * Apple's player defaults to --z-web-chrome - 1 while sticky text and
         * headers use --z-web-chrome. That lets those text layers paint sharp
         * above the player's filtered backdrop even though ordinary artwork
         * remains behind it. Keep all page content below the player glass.
         */
        z-index: calc(var(--z-web-chrome, 9901) + 1) !important;
      }
    `;
    root.dataset.noctaliaTransparentTheme = 'v4';
    return true;
  };
  const markGlassTargets = () => {
    const root = document.documentElement;
    const navigation = document.querySelector(navigationSelector);
    const player = document.querySelector(playerSelector);
    if (!root || !navigation || !player) return false;
    root.dataset.noctaliaGlassTargets = 'v4';
    return true;
  };
  if (!install()) {
    new MutationObserver((_, observer) => {
      if (install()) observer.disconnect();
    }).observe(document, {childList: true, subtree: true});
  }
  if (!markGlassTargets()) {
    const observer = new MutationObserver(() => {
      if (markGlassTargets()) observer.disconnect();
    });
    observer.observe(document, {childList: true, subtree: true});
    setTimeout(() => {
      if (!markGlassTargets()) {
        observer.disconnect();
        console.warn('[Noctalia] Apple Music glass targets were not found');
      }
    }, 10000);
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
  // GPU compositing remains enabled: production OSR requires shared textures.
  // Keep ANGLE on Vulkan so CEF allocates exportable native pixmaps instead of
  // taking its GL framebuffer path, which cannot back accelerated OSR on the
  // NVIDIA Wayland configuration used by Noctalia.
  const char* ozonePlatform = std::getenv("NOCTALIA_CEF_OZONE_PLATFORM");
  if (ozonePlatform == nullptr || ozonePlatform[0] == '\0') {
    ozonePlatform = "wayland";
  }
  cmd->AppendSwitchWithValue("ozone-platform", ozonePlatform);
  const char* angleBackend = std::getenv("NOCTALIA_CEF_ANGLE");
  if (angleBackend == nullptr || angleBackend[0] == '\0') {
    angleBackend = "vulkan";
  }
  cmd->AppendSwitchWithValue("use-angle", angleBackend);
  // ANGLE's Vulkan backend writes directly into the exportable Wayland native
  // pixmap. Chromium's separate native-Vulkan compositor is not supported by
  // Ozone Wayland here and NVIDIA services it through a shadow representation;
  // the exported DMA-BUF then remains zero to external Vulkan consumers.
  // Keep native Chromium Vulkan as an explicit diagnostic opt-in only.
  const char* nativeVulkan = std::getenv("NOCTALIA_CEF_NATIVE_VULKAN");
  if (std::strcmp(angleBackend, "vulkan") == 0
      && nativeVulkan != nullptr && std::strcmp(nativeVulkan, "1") == 0) {
    appendCommaSeparatedSwitchValue(cmd, "enable-features", "Vulkan");
  }
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
