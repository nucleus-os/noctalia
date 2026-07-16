#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace text {

  // Register a font file with the process-global fontconfig config so its family
  // becomes available to the shared Nucleus font collection. Idempotent: registering the same path twice is a
  // no-op. Returns the resolved font family name, or an empty string on failure
  // (logged). Bumps fontConfigGeneration() on the first registration of a path.
  std::string registerFontFile(const std::filesystem::path& path);

  // Monotonically increasing counter bumped whenever a new font file is
  // registered. Registration also invalidates the shared Nucleus font
  // collection immediately; this counter remains useful to higher-level
  // layout caches that want to discard resolved metrics.
  [[nodiscard]] std::uint64_t fontConfigGeneration();

} // namespace text
