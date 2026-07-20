#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Low-level transport for the EasyEffects local control socket
// ($XDG_RUNTIME_DIR/EasyEffectsServer). Shared by the preset service and the
// live equalizer service so the socket protocol lives in exactly one place.
namespace easyeffects::ipc {

  // Path to the EasyEffects control socket for the current user.
  [[nodiscard]] std::filesystem::path serverPath();

  // Whether an EasyEffects instance is reachable (its socket exists).
  [[nodiscard]] bool running();

  // Send a newline-terminated command. When readResponse is true, wait briefly
  // for the reply and return its (untrimmed) text.
  //
  // Returns nullopt when the socket is unreachable, the write fails, or a
  // response was requested but none arrived. An empty string is a valid reply.
  [[nodiscard]] std::optional<std::string> exchange(std::string_view command, bool readResponse);

  // Fire-and-forget: send a command without reading a reply. Returns false if
  // the command could not be delivered.
  bool send(std::string_view command);

  // Fire-and-forget batch. EasyEffects parses one command per line, so the whole
  // batch travels over a single connection instead of one connect/close cycle
  // per command. Returns false if the batch could not be delivered.
  bool sendAll(std::span<const std::string> commands);

} // namespace easyeffects::ipc
