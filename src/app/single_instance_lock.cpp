#include "app/single_instance_lock.h"

#include "core/log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("instance");
} // namespace

SingleInstanceLock::~SingleInstanceLock() { release(); }

SingleInstanceLock::AcquireResult SingleInstanceLock::tryAcquire() {
  m_path = resolveLockPath();

  const int fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    kLog.error("could not open lock file {}: {}", m_path, std::strerror(errno));
    return AcquireResult::Error;
  }

  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      // Another live instance holds the lock.
      ::close(fd);
      return AcquireResult::AlreadyRunning;
    }
    kLog.error("flock failed on {}: {}", m_path, std::strerror(errno));
    ::close(fd);
    return AcquireResult::Error;
  }

  m_fd = fd;
  return AcquireResult::Acquired;
}

void SingleInstanceLock::release() noexcept {
  if (m_fd >= 0) {
    // Closing the fd releases the flock. The lock file itself is intentionally
    // left in place: unlinking it would let a racing instance flock a different
    // inode than a third instance later creates, defeating the lock.
    ::close(m_fd);
    m_fd = -1;
  }
}

std::string SingleInstanceLock::resolveLockPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] == '\0') {
    return std::string("/tmp/noctalia-") + std::to_string(::getuid()) + ".lock";
  }
  return std::string(runtime) + "/noctalia.lock";
}
