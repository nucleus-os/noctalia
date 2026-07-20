#pragma once

#include <string>

// Process-lifetime single-instance guard backed by flock(2).
//
// Acquiring the lock is atomic: a second instance racing startup gets a clean
// "already held" answer with no time-of-check/time-of-use window. The kernel
// releases the lock automatically when the holding process dies, so a crashed
// instance never leaves a stale lock behind — no manual cleanup, no PID probing.
//
// The lock is global to the user runtime directory because Noctalia owns a
// process-global CEF profile. Two instances on different Wayland displays would
// otherwise collide in CEF's process singleton despite holding different shell
// locks. Display-specific IPC paths remain independent of this ownership lock.
class SingleInstanceLock {
public:
  enum class AcquireResult {
    Acquired,
    AlreadyRunning,
    Error,
  };

  SingleInstanceLock() = default;
  ~SingleInstanceLock();

  SingleInstanceLock(const SingleInstanceLock&) = delete;
  SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;

  // Attempts to acquire the lock. Locking failures are fatal to startup: running
  // unguarded could give two processes ownership of the same persistent CEF
  // profile and corrupt or destabilize the active browser process.
  [[nodiscard]] AcquireResult tryAcquire();

  [[nodiscard]] bool held() const noexcept { return m_fd >= 0; }
  [[nodiscard]] const std::string& path() const noexcept { return m_path; }

private:
  void release() noexcept;
  static std::string resolveLockPath();

  int m_fd = -1;
  std::string m_path;
};
