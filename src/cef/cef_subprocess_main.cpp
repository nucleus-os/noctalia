// Entry point for CEF's renderer/GPU/utility subprocesses. noctalia points
// CefSettings.browser_subprocess_path at this binary so the heavyweight
// noctalia main() never runs in a CEF subprocess. Its only job is to hand off
// to CefExecuteProcess with the shared app.

#include "cef/noctalia_cef_app.h"

#include "include/cef_app.h"

int main(int argc, char* argv[]) {
  CefMainArgs mainArgs(argc, argv);
  CefRefPtr<NoctaliaCefApp> app = new NoctaliaCefApp();
  return CefExecuteProcess(mainArgs, app.get(), nullptr);
}
