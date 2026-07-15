#include "render/core/texture_handle.h"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
  constexpr TextureId invalid;
  static_assert(!invalid.valid());
  static_assert(invalid.slot() == 0);
  static_assert(invalid.generation() == 0);

  constexpr TextureId first = TextureId::fromSlot(0, 1);
  static_assert(first.valid());
  static_assert(first.slot() == 0);
  static_assert(first.generation() == 1);

  constexpr TextureId reused = TextureId::fromSlot(0, 2);
  static_assert(reused.slot() == first.slot());
  static_assert(reused != first);

  constexpr TextureId anotherSlot = TextureId::fromSlot(41, 0xdeadbeefU);
  static_assert(anotherSlot.slot() == 41);
  static_assert(anotherSlot.generation() == 0xdeadbeefU);

  TextureHandle handle{.id = first, .width = 64, .height = 32};
  assert(handle.valid());
  handle = {};
  assert(!handle.valid());

  TextureGenerationAllocator generations;
  assert(generations.next() == 1);
  assert(generations.next() == 2);

  TextureGenerationAllocator nearExhaustion{std::numeric_limits<std::uint32_t>::max()};
  assert(nearExhaustion.next() == std::numeric_limits<std::uint32_t>::max());
  assert(!nearExhaustion.next().has_value());
  assert(!nearExhaustion.next().has_value());
}
