#include "cef/cef_renderer_recovery.h"

#include <cassert>

int main() {
  CefRendererRecovery recovery;
  assert(recovery.state() == CefRendererRecovery::State::Ready);

  const auto initialGeneration = recovery.generation();
  assert(recovery.onTerminated() == CefRendererRecovery::Action::Reload);
  assert(recovery.state() == CefRendererRecovery::State::WaitingForRenderView);
  assert(recovery.generation() == initialGeneration + 1);
  const auto firstRecoveryGeneration = recovery.generation();
  assert(recovery.isPending(firstRecoveryGeneration));
  assert(!recovery.isPending(initialGeneration));

  // A crash during the replacement renderer's startup must not create an
  // unbounded reload loop. It also invalidates the already-posted reload.
  assert(recovery.onTerminated() == CefRendererRecovery::Action::Stop);
  assert(recovery.state() == CefRendererRecovery::State::Failed);
  assert(!recovery.isPending(firstRecoveryGeneration));
  assert(recovery.onTerminated() == CefRendererRecovery::Action::Stop);
  assert(recovery.state() == CefRendererRecovery::State::Failed);

  // CEF may still produce a healthy render view through its own recovery. A
  // real ready callback is authoritative and restores the normal lifecycle.
  recovery.onRenderViewReady();
  assert(recovery.state() == CefRendererRecovery::State::Ready);
  assert(!recovery.isPending(recovery.generation()));
  assert(recovery.onTerminated() == CefRendererRecovery::Action::Reload);

  const auto pendingBeforeReset = recovery.generation();
  assert(recovery.isPending(pendingBeforeReset));
  recovery.reset();
  assert(recovery.state() == CefRendererRecovery::State::Ready);
  assert(!recovery.isPending(pendingBeforeReset));
}
