#pragma once

#include "include/cef_app.h"

#include <cstdint>
#include <functional>
#include <utility>

// CefApp shared by the browser process and the renderer/GPU/utility
// subprocesses (the helper binary constructs one too). In the browser process
// CefService installs a schedule-work callback so CEF's external message pump
// can be driven from noctalia's poll loop instead of a busy timer.
class NoctaliaCefApp : public CefApp, public CefBrowserProcessHandler {
public:
  using ScheduleWorkCallback = std::function<void(std::int64_t delayMs)>;

  NoctaliaCefApp() = default;
  explicit NoctaliaCefApp(ScheduleWorkCallback cb) : m_scheduleWork(std::move(cb)) {}

  // CefApp
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnBeforeCommandLineProcessing(const CefString& processType, CefRefPtr<CefCommandLine> cmd) override;

  // CefBrowserProcessHandler — may fire on any CEF thread; the callback marshals
  // to the main loop.
  void OnScheduleMessagePumpWork(std::int64_t delayMs) override;

private:
  ScheduleWorkCallback m_scheduleWork;
  IMPLEMENT_REFCOUNTING(NoctaliaCefApp);
};
