#pragma once

#include "include/cef_app.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

// Linux Chromium discovers Widevine before starting its zygote by reading a
// profile-local hint file. Accept either the WidevineCdm base directory or its
// linux_x64 library directory and provision that file before CefInitialize.
[[nodiscard]] bool prepareNoctaliaWidevineHint(
    const std::string& rootCachePath, const std::string& widevinePath, std::string& error
);

// CefApp shared by the browser process and the renderer/GPU/utility
// subprocesses (the helper binary constructs one too). In the browser process
// CefService installs a schedule-work callback so CEF's external message pump
// can be driven from noctalia's poll loop instead of a busy timer.
class NoctaliaCefApp : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
  using ScheduleWorkCallback = std::function<void(std::int64_t delayMs)>;

  NoctaliaCefApp() = default;
  explicit NoctaliaCefApp(ScheduleWorkCallback cb) : m_scheduleWork(std::move(cb)) {}

  // CefApp
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
  void OnBeforeCommandLineProcessing(const CefString& processType, CefRefPtr<CefCommandLine> cmd) override;

  // CefBrowserProcessHandler
  void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> cmd) override;
  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<CefCommandLine> commandLine, const CefString& currentDirectory
  ) override;
  // May fire on any CEF thread; the callback marshals to the main loop.
  void OnScheduleMessagePumpWork(std::int64_t delayMs) override;

  // CefRenderProcessHandler
  void OnContextCreated(
      CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
  ) override;

private:
  ScheduleWorkCallback m_scheduleWork;
  IMPLEMENT_REFCOUNTING(NoctaliaCefApp);
};
