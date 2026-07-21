#include "cef/noctalia_cef_app.h"

#include "cef/site_integrations/site_integration.h"
#include "cef/site_integrations/site_integration_policy.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace {

class AppleMusicMediaStateV8Handler final : public CefV8Handler {
public:
  explicit AppleMusicMediaStateV8Handler(CefRefPtr<CefFrame> frame) : m_frame(std::move(frame)) {}

  bool Execute(
      const CefString& /*name*/, CefRefPtr<CefV8Value> /*object*/, const CefV8ValueList& arguments,
      CefRefPtr<CefV8Value>& retval, CefString& exception
  ) override {
    if (arguments.size() != 5 || !arguments[0]->IsBool()) {
      exception = "invalid Apple Music media-state event";
      return true;
    }
    for (std::size_t i = 1; i < arguments.size(); ++i) {
      if (!arguments[i]->IsString()) {
        exception = "invalid Apple Music media-state field";
        return true;
      }
    }
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("noctalia.apple-music.media-state");
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetBool(0, arguments[0]->GetBoolValue());
    for (std::size_t i = 1; i < arguments.size(); ++i) {
      args->SetString(i, arguments[i]->GetStringValue());
    }
    if (m_frame != nullptr) {
      m_frame->SendProcessMessage(PID_BROWSER, message);
    }
    retval = CefV8Value::CreateBool(true);
    return true;
  }

private:
  CefRefPtr<CefFrame> m_frame;
  IMPLEMENT_REFCOUNTING(AppleMusicMediaStateV8Handler);
};


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
    CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
) {
  if (!frame->IsMain()) {
    return;
  }
  if (siteIntegrationForUrl(frame->GetURL().ToString()) == SiteIntegration::AppleMusic && context != nullptr) {
    CefRefPtr<CefV8Value> function = CefV8Value::CreateFunction(
        "__noctaliaReportAppleMusicMediaState", new AppleMusicMediaStateV8Handler(frame)
    );
    context->GetGlobal()->SetValue(
        "__noctaliaReportAppleMusicMediaState", function,
        static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE)
    );
  }
  installWebPanelSiteIntegration(frame);
}
