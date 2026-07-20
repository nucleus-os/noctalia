#include "system/easyeffects_ipc.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace easyeffects::ipc {

std::filesystem::path serverPath() {
  if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir != nullptr && runtimeDir[0] != '\0') {
    return std::filesystem::path(runtimeDir) / "EasyEffectsServer";
  }
  return std::filesystem::path("/run/user") / std::to_string(::getuid()) / "EasyEffectsServer";
}

bool running() {
  std::error_code ec;
  return std::filesystem::exists(serverPath(), ec) && !ec;
}

namespace {

  // Shared connect/write/read core. `payload` is already newline-terminated.
  std::optional<std::string> transfer(const std::string& payload, bool readResponse) {
    const std::filesystem::path path = serverPath();
    const std::string nativePath = path.string();
    if (nativePath.empty() || nativePath.size() >= sizeof(sockaddr_un::sun_path)) {
      return std::nullopt;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, nativePath.c_str(), sizeof(addr.sun_path) - 1);

    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      const char* data = payload.data();
      std::size_t remaining = payload.size();
      ok = true;
      while (remaining > 0) {
        const ssize_t n = ::send(fd, data, remaining, MSG_NOSIGNAL);
        if (n > 0) {
          const auto written = static_cast<std::size_t>(n);
          data += written;
          remaining -= written;
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        ok = false;
        break;
      }
    }

    std::string response;
    bool receivedResponse = false;
    if (ok && readResponse) {
      (void)::shutdown(fd, SHUT_WR);
      std::array<char, 512> buffer{};
      while (true) {
        pollfd pfd{.fd = fd, .events = POLLIN | POLLHUP, .revents = 0};
        const int pollRc = ::poll(&pfd, 1, 250);
        if (pollRc < 0 && errno == EINTR) {
          continue;
        }
        if (pollRc <= 0) {
          break;
        }

        const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n > 0) {
          receivedResponse = true;
          response.append(buffer.data(), static_cast<std::size_t>(n));
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
    }

    ::close(fd);
    if (!ok) {
      return std::nullopt;
    }
    if (readResponse && !receivedResponse) {
      return std::nullopt;
    }
    return response;
  }

} // namespace

std::optional<std::string> exchange(std::string_view command, bool readResponse) {
  std::string payload(command);
  payload.push_back('\n');
  return transfer(payload, readResponse);
}

bool sendAll(std::span<const std::string> commands) {
  if (commands.empty()) {
    return true;
  }
  std::string payload;
  for (const std::string& command : commands) {
    payload.append(command);
    payload.push_back('\n');
  }
  return transfer(payload, false).has_value();
}

bool send(std::string_view command) {
  return exchange(command, false).has_value();
}

} // namespace easyeffects::ipc
