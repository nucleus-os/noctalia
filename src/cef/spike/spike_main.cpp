// Phase 1 de-risking spike for the in-shell Apple Music (CEF) project.
//
// This is a STANDALONE, throwaway-OK executable — it does NOT link into the
// noctalia binary. Its only job is to answer the make-or-break question before
// we commit to full integration:
//
//   1. Does CEF build + link against this toolchain and initialize in
//      off-screen-rendering (OSR / windowless) mode?
//   2. Does an ordinary web page paint BGRA frames via OnPaint?
//   3. **Does a Widevine-DRM protected stream actually decode and paint?**
//      (Apple Music needs this; if it fails, the whole approach is blocked.)
//
// It renders headless and dumps PNG snapshots to a temp dir so the frames —
// including the DRM video — can be inspected visually. It also runs an EME
// capability probe and captures console output.
//
// Single-binary multi-process model: the same executable re-execs itself for
// CEF's renderer/GPU/utility subprocesses via CefExecuteProcess().
//
// Env overrides:
//   NOCTALIA_CEF_URL       page to load (default: a public Widevine DRM demo)
//   NOCTALIA_CEF_SECONDS   how long to run the message loop (default: 30)
//   NOCTALIA_CEF_OUT       snapshot output dir (default: /tmp/noctalia-cef-spike)
//   NOCTALIA_CEF_WIDEVINE  dir containing libwidevinecdm.so (default: Chrome's)

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string envOr(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v != nullptr && v[0] != '\0') ? std::string(v) : fallback;
}

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// Chrome ships a Widevine CDM we can point CEF at.
const std::string kDefaultWidevine = "/opt/google/chrome/WidevineCdm/_platform_specific/linux_x64";
const std::string kDefaultUrl = "https://bitmovin.com/demos/drm";

// ── Shared state written by the render handler, read by main ─────────────────
struct FrameSink {
  std::mutex mutex;
  std::vector<unsigned char> bgra; // latest full frame
  int width = 0;
  int height = 0;
  std::atomic<long> paintCount{0};
  std::atomic<unsigned long> lastHash{0};
  std::atomic<long> distinctFrames{0};
};

FrameSink g_sink;
std::string g_outDir;

unsigned long hashCenter(const unsigned char* b, int w, int h) {
  // FNV-1a over a center crop — used to detect motion (video actually playing).
  unsigned long hash = 1469598103934665603UL;
  const int x0 = w / 4, x1 = w * 3 / 4, y0 = h / 4, y1 = h * 3 / 4;
  for (int y = y0; y < y1; y += 4) {
    const unsigned char* row = b + static_cast<size_t>(y) * w * 4;
    for (int x = x0; x < x1; x += 4) {
      hash = (hash ^ row[x * 4]) * 1099511628211UL;
    }
  }
  return hash;
}

void writePng(const std::string& path, const unsigned char* bgra, int w, int h) {
  // stb writes RGBA; convert BGRA -> RGBA.
  std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i + 0] = bgra[i + 2];
    rgba[i + 1] = bgra[i + 1];
    rgba[i + 2] = bgra[i + 0];
    rgba[i + 3] = 255; // force opaque for inspection
  }
  stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4);
}

// ── Client: render handler + lifespan + load + console ───────────────────────
class SpikeClient : public CefClient,
                    public CefRenderHandler,
                    public CefLifeSpanHandler,
                    public CefLoadHandler,
                    public CefDisplayHandler {
public:
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  // CefRenderHandler
  void GetViewRect(CefRefPtr<CefBrowser> /*browser*/, CefRect& rect) override {
    rect = CefRect(0, 0, kWidth, kHeight);
  }

  void OnPaint(
      CefRefPtr<CefBrowser> /*browser*/, PaintElementType type, const RectList& /*dirtyRects*/,
      const void* buffer, int width, int height
  ) override {
    if (type != PET_VIEW) {
      return; // ignore popup widgets for the spike
    }
    const auto* b = static_cast<const unsigned char*>(buffer);
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    {
      std::scoped_lock lock(g_sink.mutex);
      g_sink.bgra.assign(b, b + bytes);
      g_sink.width = width;
      g_sink.height = height;
    }
    const unsigned long h = hashCenter(b, width, height);
    if (h != g_sink.lastHash.exchange(h)) {
      g_sink.distinctFrames.fetch_add(1);
    }
    g_sink.paintCount.fetch_add(1);
  }

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    m_browser = browser;
    fprintf(stderr, "[spike] browser created\n");
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override { m_browser = nullptr; }

  // CefLoadHandler
  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override {
    if (!frame->IsMain()) {
      return;
    }
    fprintf(stderr, "[spike] load end (http %d): %s\n", httpStatusCode, frame->GetURL().ToString().c_str());
    // EME capability probe, repeated — Widevine may register asynchronously
    // (component updater) a few seconds after startup.
    // One-shot codec support report (compile-time capability, independent of DRM).
    frame->ExecuteJavaScript(
        "(function(){var v=document.createElement('video');"
        "console.log('CODEC aac(canPlay): \"'+v.canPlayType('audio/mp4; codecs=\"mp4a.40.2\"')+'\"');"
        "console.log('CODEC h264(canPlay): \"'+v.canPlayType('video/mp4; codecs=\"avc1.42E01E\"')+'\"');"
        "console.log('CODEC aac(MSE): '+(window.MediaSource&&MediaSource.isTypeSupported('audio/mp4; codecs=\"mp4a.40.2\"')));"
        "console.log('CODEC h264(MSE): '+(window.MediaSource&&MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.42E01E\"')));"
        "console.log('CODEC vp9(MSE): '+(window.MediaSource&&MediaSource.isTypeSupported('video/webm; codecs=\"vp9\"')));"
        "})();",
        frame->GetURL(), 0
    );
    frame->ExecuteJavaScript(
        "(function(){let n=0;function one(tag,cfg){"
        "navigator.requestMediaKeySystemAccess('com.widevine.alpha',cfg)"
        ".then(()=>console.log('EME-PROBE '+tag+': OK (attempt '+n+')'))"
        ".catch(e=>console.log('EME-PROBE '+tag+': FAIL (attempt '+n+') '+e.name));}"
        "function probe(){"
        // free codecs (VP9/Opus) — isolates Widevine presence from proprietary codecs
        "one('vp9',[{initDataTypes:['cenc'],"
        "videoCapabilities:[{contentType:'video/webm;codecs=\"vp9\"'}]}]);"
        // proprietary (H.264/AAC) — what Apple Music actually needs
        "one('h264',[{initDataTypes:['cenc'],"
        "audioCapabilities:[{contentType:'audio/mp4;codecs=\"mp4a.40.2\"'}],"
        "videoCapabilities:[{contentType:'video/mp4;codecs=\"avc1.42E01E\"'}]}]);"
        "if(++n<10)setTimeout(probe,3000);}probe();})();",
        frame->GetURL(), 0
    );
    m_loaded = true;
  }

  void OnLoadError(
      CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
      const CefString& errorText, const CefString& failedUrl
  ) override {
    if (frame->IsMain()) {
      fprintf(
          stderr, "[spike] LOAD ERROR %d (%s): %s\n", errorCode, errorText.ToString().c_str(),
          failedUrl.ToString().c_str()
      );
    }
  }

  // CefDisplayHandler — surface page console output (EME probe + player events).
  bool OnConsoleMessage(
      CefRefPtr<CefBrowser> /*browser*/, cef_log_severity_t /*level*/, const CefString& message,
      const CefString& /*source*/, int /*line*/
  ) override {
    fprintf(stderr, "[page] %s\n", message.ToString().c_str());
    return false;
  }

  CefRefPtr<CefBrowser> browser() const { return m_browser; }
  bool loaded() const { return m_loaded; }

private:
  CefRefPtr<CefBrowser> m_browser;
  bool m_loaded = false;
  IMPLEMENT_REFCOUNTING(SpikeClient);
};

// ── App: command-line flags + subprocess plumbing ────────────────────────────
class SpikeApp : public CefApp, public CefBrowserProcessHandler {
public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

  void OnBeforeCommandLineProcessing(const CefString& processType, CefRefPtr<CefCommandLine> cmd) override {
    // Applies to every process (browser + subprocesses).
    if (processType.empty()) {
      // Browser process only.
      cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
      // OSR: keep compositing off the GPU so frames read back reliably headless.
      cmd->AppendSwitch("disable-gpu-compositing");
      const std::string widevine = envOr("NOCTALIA_CEF_WIDEVINE", kDefaultWidevine);
      if (!widevine.empty()) {
        cmd->AppendSwitchWithValue("widevine-cdm-path", widevine);
      }
    }
  }

  IMPLEMENT_REFCOUNTING(SpikeApp);
};

void ensureDir(const std::string& path) {
  ::mkdir(path.c_str(), 0755);
}

} // namespace

int main(int argc, char* argv[]) {
  CefMainArgs mainArgs(argc, argv);
  CefRefPtr<SpikeApp> app = new SpikeApp();

  // Subprocess dispatch: for renderer/GPU/utility invocations this returns >= 0
  // and we exit immediately. The browser process gets -1 and continues.
  const int exitCode = CefExecuteProcess(mainArgs, app.get(), nullptr);
  if (exitCode >= 0) {
    return exitCode;
  }

  g_outDir = envOr("NOCTALIA_CEF_OUT", "/tmp/noctalia-cef-spike");
  ensureDir(g_outDir);
  const std::string url = envOr("NOCTALIA_CEF_URL", kDefaultUrl);
  const int seconds = std::atoi(envOr("NOCTALIA_CEF_SECONDS", "30").c_str());

  CefSettings settings;
  settings.no_sandbox = true;
  settings.windowless_rendering_enabled = true;
  settings.multi_threaded_message_loop = false;
  settings.external_message_pump = false;
  settings.log_severity = LOGSEVERITY_WARNING;
  CefString(&settings.root_cache_path).FromString("/tmp/noctalia-cef-spike-cache");
#ifdef NOCTALIA_CEF_DIR
  // ICU (icudtl.dat) initializes very early — it must sit next to libcef.so in
  // Release/. The fetch/install step colocates the Resources/ payload there, so
  // point both resource paths at Release/.
  const std::string cefDir = NOCTALIA_CEF_DIR;
  CefString(&settings.resources_dir_path).FromString(cefDir + "/Release");
  CefString(&settings.locales_dir_path).FromString(cefDir + "/Release/locales");
#endif

  if (!CefInitialize(mainArgs, settings, app.get(), nullptr)) {
    fprintf(stderr, "[spike] CefInitialize FAILED\n");
    return 1;
  }
  fprintf(stderr, "[spike] CefInitialize OK; loading %s\n", url.c_str());

  CefRefPtr<SpikeClient> client = new SpikeClient();

  CefWindowInfo windowInfo;
  windowInfo.SetAsWindowless(0); // no parent window
  CefBrowserSettings browserSettings;
  browserSettings.windowless_frame_rate = 30;

  CefBrowserHost::CreateBrowser(windowInfo, client, url, browserSettings, nullptr, nullptr);

  // Cooperative pump: drive CEF and snapshot periodically for `seconds`.
  const int iterations = seconds * 100; // 10ms cadence
  int lastSnapshotIter = -1000;
  int snapshotIndex = 0;
  for (int i = 0; i < iterations; ++i) {
    CefDoMessageLoopWork();
    usleep(10 * 1000);

    // Snapshot every ~2s once something has painted.
    if (g_sink.paintCount.load() > 0 && i - lastSnapshotIter >= 200) {
      lastSnapshotIter = i;
      std::vector<unsigned char> copy;
      int w = 0, h = 0;
      {
        std::scoped_lock lock(g_sink.mutex);
        copy = g_sink.bgra;
        w = g_sink.width;
        h = g_sink.height;
      }
      if (!copy.empty()) {
        char name[256];
        std::snprintf(name, sizeof(name), "%s/frame_%02d.png", g_outDir.c_str(), snapshotIndex++);
        writePng(name, copy.data(), w, h);
        fprintf(
            stderr, "[spike] t=%2ds  paints=%ld  distinct=%ld  -> %s\n", i / 100,
            g_sink.paintCount.load(), g_sink.distinctFrames.load(), name
        );
      }
    }
  }

  // Report card.
  const long paints = g_sink.paintCount.load();
  const long distinct = g_sink.distinctFrames.load();
  fprintf(stderr, "\n===== SPIKE REPORT =====\n");
  fprintf(stderr, "url:             %s\n", url.c_str());
  fprintf(stderr, "total paints:    %ld\n", paints);
  fprintf(stderr, "distinct frames: %ld  (motion => video is decoding+painting)\n", distinct);
  fprintf(stderr, "snapshots in:    %s\n", g_outDir.c_str());
  fprintf(stderr, "verdict(render): %s\n", paints > 0 ? "PASS (page painted)" : "FAIL (no frames)");
  fprintf(stderr, "verdict(motion): %s\n", distinct > 5 ? "PASS (frames changing)" : "INCONCLUSIVE");
  fprintf(stderr, "Check [page] EME-PROBE line above for Widevine capability.\n");
  fprintf(stderr, "Inspect the PNGs to confirm the DRM video actually renders.\n");
  fprintf(stderr, "========================\n");

  if (client->browser()) {
    client->browser()->GetHost()->CloseBrowser(true);
  }
  for (int i = 0; i < 50; ++i) {
    CefDoMessageLoopWork();
    usleep(10 * 1000);
  }
  CefShutdown();
  return 0;
}
