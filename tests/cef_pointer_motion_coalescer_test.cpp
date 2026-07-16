#include "cef/cef_pointer_motion_coalescer.h"

#include <cassert>

int main() {
  CefPointerMotionCoalescer coalescer;

  assert(coalescer.queue({10, 20, 0}));
  assert(coalescer.queue({11, 21, 0}));
  assert(coalescer.pending());
  assert((coalescer.take() == CefPointerMotion{11, 21, 0}));
  assert(!coalescer.pending());

  // Repeated integer-pixel positions do not reach CEF again.
  assert(!coalescer.queue({11, 21, 0}));
  assert(!coalescer.take());

  // Modifiers are part of the event identity.
  assert(coalescer.queue({11, 21, 1}));
  assert((coalescer.take() == CefPointerMotion{11, 21, 1}));

  // Discarding a queued move retains duplicate suppression for the last event.
  assert(coalescer.queue({30, 40, 0}));
  coalescer.discardPending();
  assert(!coalescer.pending());

  // Leave/reset permits an enter at the same coordinates to be forwarded.
  coalescer.reset();
  assert(coalescer.queue({11, 21, 1}));
  assert((coalescer.take() == CefPointerMotion{11, 21, 1}));
}
