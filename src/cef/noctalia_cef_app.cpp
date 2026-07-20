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

// Apple Music's timed-lyrics component measures its host only in
// componentDidLoad(). Noctalia keeps the same document and component instance
// while moving it between a panel and a fullscreen toplevel, so a viewport
// resize otherwise leaves both its spacer geometry and scrollTop stale.
//
// Capture a semantic line anchor before the Wayland resize, repair the
// component's own hostHeight state after it, and restore either the active
// timed line (auto-follow mode) or the user's visible reading position. The
// bridge deliberately uses stable custom-element names and public DOM/shadow
// structure instead of Apple Music's generated CSS class names.
constexpr std::string_view kAppleMusicLyricsResizeBridgeScript = R"JS(
(() => {
  const bridgeKey = '__noctaliaAppleMusicLyricsResizeV1';
  if (window[bridgeKey]) return;

  let pending = null;
  let resizeSerial = 0;

  const nextFrame = callback => requestAnimationFrame(() => requestAnimationFrame(callback));
  const normalizedText = line => {
    const text = line?.shadowRoot?.textContent ?? line?.textContent ?? '';
    return text.replace(/\s+/g, ' ').trim();
  };

  const findTimedLyrics = () => {
    const modal = document.querySelector('[data-testid="lyrics-fullscreen-modal"]');
    const lyrics = modal?.querySelector('amp-lyrics') ?? document.querySelector('amp-lyrics');
    const lyricsRoot = lyrics?.shadowRoot;
    const scroller = lyricsRoot?.querySelector('amp-lyrics-display-time-synced');
    const lineRoot = scroller?.shadowRoot;
    if (!lyrics || !scroller || !lineRoot) return null;
    return {
      lyrics,
      scroller,
      lineRoot
    };
  };

  const linesIn = lineRoot =>
    Array.from(lineRoot.querySelectorAll('amp-lyrics-display-synced-line'));

  const nearestVisibleLine = (state, lines) => {
    if (!lines.length) return null;
    const scrollerRect = state.scroller.getBoundingClientRect();
    const referenceY = scrollerRect.top + state.scroller.clientHeight * 0.4;
    return lines.reduce((nearest, line) => {
      const rect = line.getBoundingClientRect();
      const distance = Math.abs(rect.top + rect.height * 0.5 - referenceY);
      return !nearest || distance < nearest.distance ? {line, distance} : nearest;
    }, null)?.line ?? null;
  };

  const resolveAnchor = (snapshot, state) => {
    const lines = linesIn(state.lineRoot);
    if (snapshot.autoFollow) {
      return state.lineRoot.querySelector("[is-current='']") ?? nearestVisibleLine(state, lines);
    }
    if (snapshot.anchor?.isConnected && snapshot.anchor.getRootNode() === state.lineRoot) {
      return snapshot.anchor;
    }
    if (Number.isInteger(snapshot.lineIndex)) {
      const byProperty = lines.find(line => Number(line.lineIndex) === snapshot.lineIndex);
      if (byProperty) return byProperty;
      if (snapshot.lineIndex >= 0 && snapshot.lineIndex < lines.length) {
        return lines[snapshot.lineIndex];
      }
    }
    if (snapshot.text) {
      const byText = lines.find(line => normalizedText(line) === snapshot.text);
      if (byText) return byText;
    }
    return nearestVisibleLine(state, lines);
  };

  const updateComponentGeometry = state => {
    const height = state.scroller.getBoundingClientRect().height;
    if (Number.isFinite(height) && height > 0
        && Math.abs(Number(state.scroller.hostHeight) - height) > 0.5) {
      // hostHeight is Stencil state with Apple's own adjustTopOffset watcher.
      // Updating it causes the component to rebuild both spacer divs using the
      // new viewport instead of carrying panel dimensions into fullscreen.
      state.scroller.hostHeight = height;
    }
  };

  const restoreAnchor = snapshot => {
    const state = findTimedLyrics();
    if (!state || state.scroller.clientHeight <= 0) return;

    const anchor = resolveAnchor(snapshot, state);
    if (!anchor) {
      const maxScroll = Math.max(0, state.scroller.scrollHeight - state.scroller.clientHeight);
      state.scroller.scrollTop = snapshot.scrollFraction * maxScroll;
      return;
    }

    const scrollerRect = state.scroller.getBoundingClientRect();
    const anchorY = anchor.getBoundingClientRect().top - scrollerRect.top;
    let desiredY;
    if (snapshot.autoFollow) {
      const topOffset = Number(state.scroller.topOffset);
      const margin = Number(state.scroller.scrollTopMargin);
      desiredY = (Number.isFinite(topOffset) ? topOffset : state.scroller.clientHeight * 0.4)
          + (Number.isFinite(margin) ? margin : 55);
    } else {
      desiredY = snapshot.relativeY * state.scroller.clientHeight;
    }
    desiredY = Math.max(0, Math.min(state.scroller.clientHeight, desiredY));
    state.scroller.scrollTop += anchorY - desiredY;
  };

  const repairAfterResize = (snapshot, serial) => {
    const state = findTimedLyrics();
    if (!state || pending !== snapshot || serial !== resizeSerial) return;
    updateComponentGeometry(state);
    // Stencil schedules its render after hostHeight changes. Wait through two
    // animation frames before reading line geometry and applying the anchor.
    nextFrame(() => {
      if (pending !== snapshot || serial !== resizeSerial) return;
      restoreAnchor(snapshot);
    });
  };

  const onResize = () => {
    const snapshot = pending;
    if (!snapshot) return;
    const serial = ++resizeSerial;
    repairAfterResize(snapshot, serial);

    clearTimeout(snapshot.settleTimer);
    snapshot.settleTimer = setTimeout(() => {
      if (pending !== snapshot || serial !== resizeSerial) return;
      const state = findTimedLyrics();
      if (state) updateComponentGeometry(state);
      nextFrame(() => {
        if (pending !== snapshot || serial !== resizeSerial) return;
        restoreAnchor(snapshot);
        clearTimeout(snapshot.expiryTimer);
        pending = null;
      });
    }, 180);
  };

  const capture = () => {
    const state = findTimedLyrics();
    if (!state || state.scroller.clientHeight <= 0) {
      pending = null;
      return false;
    }

    if (pending) {
      clearTimeout(pending.settleTimer);
      clearTimeout(pending.expiryTimer);
    }
    const lines = linesIn(state.lineRoot);
    // The component exposes the same state that drives Apple's
    // scrollBehaviorChange event; avoid depending on a generated wrapper
    // class to distinguish timed auto-follow from a user's manual reading.
    const autoFollow = state.scroller.scrollBehavior !== 'user';
    const current = state.lineRoot.querySelector("[is-current='']");
    const anchor = autoFollow && current ? current : nearestVisibleLine(state, lines);
    const scrollerRect = state.scroller.getBoundingClientRect();
    const anchorRect = anchor?.getBoundingClientRect();
    const arrayIndex = anchor ? lines.indexOf(anchor) : -1;
    const propertyIndex = Number(anchor?.lineIndex);
    const maxScroll = Math.max(1, state.scroller.scrollHeight - state.scroller.clientHeight);

    const snapshot = {
      autoFollow,
      anchor,
      lineIndex: Number.isInteger(propertyIndex) ? propertyIndex : arrayIndex,
      text: normalizedText(anchor),
      relativeY: anchorRect
          ? (anchorRect.top - scrollerRect.top) / state.scroller.clientHeight
          : 0.4,
      scrollFraction: state.scroller.scrollTop / maxScroll,
      settleTimer: 0,
      expiryTimer: 0
    };
    snapshot.expiryTimer = setTimeout(() => {
      if (pending === snapshot) pending = null;
    }, 2000);
    pending = snapshot;
    return true;
  };

  addEventListener('resize', onResize, {capture: true});
  window[bridgeKey] = Object.freeze({capture});
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
  if (const char* validation = std::getenv("NOCTALIA_VULKAN_VALIDATION");
      validation != nullptr && validation[0] == '1') {
    appendCommaSeparatedSwitchValue(
        cmd, "enable-features", "SkiaGraphite:dawn_skip_validation/false/dawn_backend_validation/true"
    );
    cmd->AppendSwitchWithValue("enable-dawn-backend-validation", "full");
  }
  // Viz renders with Graphite/Dawn/Vulkan into the exportable native pixmap.
  // ANGLE remains Vulkan-backed for WebGL and other GL-originated content.
  cmd->AppendSwitchWithValue("ozone-platform", "wayland");
  cmd->AppendSwitch("enable-skia-graphite");
  cmd->AppendSwitchWithValue("skia-graphite-dawn-backend", "vulkan");
  cmd->AppendSwitch("require-skia-graphite-dawn-vulkan");
  cmd->AppendSwitchWithValue("use-angle", "vulkan");
}

void NoctaliaCefApp::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> cmd) {
  // Forward the compositor-selected GPU explicitly so Dawn, ANGLE, native
  // Vulkan, and the Wayland native-pixmap allocator use the same device.
  if (const char* uuid = std::getenv("NOCTALIA_CEF_VULKAN_DEVICE_UUID");
      uuid != nullptr && uuid[0] != '\0') {
    cmd->AppendSwitchWithValue("vulkan-device-uuid", uuid);
  }
  if (const char* renderNode = std::getenv("NOCTALIA_CEF_DRM_RENDER_NODE");
      renderNode != nullptr && renderNode[0] != '\0') {
    cmd->AppendSwitchWithValue("render-node-override", renderNode);
  }
}

bool NoctaliaCefApp::OnAlreadyRunningAppRelaunch(
    CefRefPtr<CefCommandLine> /*commandLine*/, const CefString& /*currentDirectory*/
) {
  // Noctalia owns its window and application lifecycle. Returning false would
  // make CEF's Chrome runtime create a default native browser window in the
  // existing process, violating the windowless-only rendering contract.
  return true;
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
    frame->ExecuteJavaScript(std::string(kAppleMusicLyricsResizeBridgeScript), frame->GetURL(), 0);
  }
}
