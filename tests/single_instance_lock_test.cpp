#include "app/single_instance_lock.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  class EnvironmentRestore {
  public:
    explicit EnvironmentRestore(const char* name) : m_name(name) {
      if (const char* value = std::getenv(name)) {
        m_value = value;
      }
    }

    ~EnvironmentRestore() {
      if (m_value.has_value()) {
        ::setenv(m_name.c_str(), m_value->c_str(), 1);
      } else {
        ::unsetenv(m_name.c_str());
      }
    }

  private:
    std::string m_name;
    std::optional<std::string> m_value;
  };

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "single_instance_lock_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  EnvironmentRestore restoreRuntime("XDG_RUNTIME_DIR");
  EnvironmentRestore restoreDisplay("WAYLAND_DISPLAY");

  const auto root =
      std::filesystem::temp_directory_path() / ("noctalia-single-instance-" + std::to_string(::getpid()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  if (!expect(!error, "failed to create temporary runtime directory")) {
    return 1;
  }

  ::setenv("XDG_RUNTIME_DIR", root.c_str(), 1);
  ::setenv("WAYLAND_DISPLAY", "wayland-1", 1);

  bool ok = true;
  {
    SingleInstanceLock first;
    ok = expect(
             first.tryAcquire() == SingleInstanceLock::AcquireResult::Acquired,
             "first instance did not acquire the lock"
         )
        && ok;
    ok = expect(first.path() == (root / "noctalia.lock").string(), "lock path was not global") && ok;

    ::setenv("WAYLAND_DISPLAY", "wayland-2", 1);
    SingleInstanceLock second;
    ok = expect(
             second.tryAcquire() == SingleInstanceLock::AcquireResult::AlreadyRunning,
             "different Wayland displays did not contend for the same lock"
         )
        && ok;
  }

  {
    SingleInstanceLock afterRelease;
    ok = expect(
             afterRelease.tryAcquire() == SingleInstanceLock::AcquireResult::Acquired,
             "lock was not released with its owner"
         )
        && ok;
  }

  std::filesystem::remove_all(root, error);
  const auto missingRuntime = root / "missing";
  ::setenv("XDG_RUNTIME_DIR", missingRuntime.c_str(), 1);
  SingleInstanceLock failed;
  ok = expect(
           failed.tryAcquire() == SingleInstanceLock::AcquireResult::Error,
           "unusable runtime directory did not fail closed"
       )
      && ok;

  std::filesystem::remove_all(root, error);
  return ok ? 0 : 1;
}
