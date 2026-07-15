#pragma once

#include <cstdint>
#include <limits>
#include <optional>

// Issues generations monotonically for the lifetime of a GraphicsDevice.
// Generation zero is reserved, and exhaustion fails instead of wrapping and
// allowing a stale TextureId to alias a newly allocated texture.
class TextureGenerationAllocator {
public:
  explicit constexpr TextureGenerationAllocator(std::uint64_t next = 1) noexcept : m_next(next) {}

  [[nodiscard]] constexpr std::optional<std::uint32_t> next() noexcept {
    if (m_next == 0 || m_next > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(m_next++);
  }

private:
  std::uint64_t m_next;
};

class TextureId {
public:
  constexpr TextureId() noexcept = default;
  explicit constexpr TextureId(std::uint64_t value) noexcept : m_value(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return m_value; }
  [[nodiscard]] constexpr bool valid() const noexcept { return m_value != 0; }
  [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

  [[nodiscard]] static constexpr TextureId fromSlot(std::uint32_t slot, std::uint32_t generation) noexcept {
    return TextureId{(static_cast<std::uint64_t>(generation) << 32U) | (static_cast<std::uint64_t>(slot) + 1U)};
  }
  [[nodiscard]] constexpr std::uint32_t slot() const noexcept {
    return valid() ? static_cast<std::uint32_t>((m_value & 0xffffffffULL) - 1ULL) : 0;
  }
  [[nodiscard]] constexpr std::uint32_t generation() const noexcept {
    return static_cast<std::uint32_t>(m_value >> 32U);
  }

  friend constexpr bool operator==(TextureId lhs, TextureId rhs) noexcept = default;
  friend constexpr bool operator==(TextureId lhs, std::uint64_t rhs) noexcept { return lhs.m_value == rhs; }
  friend constexpr bool operator==(std::uint64_t lhs, TextureId rhs) noexcept { return lhs == rhs.m_value; }

private:
  std::uint64_t m_value = 0;
};

struct TextureHandle {
  TextureId id;
  int width = 0;
  int height = 0;
  std::uint64_t generation = 0;

  [[nodiscard]] constexpr bool valid() const noexcept { return id.valid(); }
};
