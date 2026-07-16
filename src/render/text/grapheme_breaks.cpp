#include "render/text/grapheme_breaks.h"

#include <nucleus/text/TextLayoutBuilder.hpp>

#include <cstdint>

namespace text {

std::vector<std::size_t> graphemeBreaks(std::string_view text) {
  nucleus::text::TextLayoutService service;
  std::uint32_t count = 0;
  if (service.graphemeBreaks({text.data(), text.size()}, nullptr, 0, &count)) {
    std::vector<std::uint32_t> offsets(count);
    if (service.graphemeBreaks({text.data(), text.size()}, offsets.data(), offsets.size(), &count)) {
      std::vector<std::size_t> result;
      result.reserve(count);
      for (std::uint32_t offset : offsets) {
        result.push_back(offset);
      }
      if (result.empty() || result.front() != 0) {
        result.insert(result.begin(), 0);
      }
      if (result.back() != text.size()) {
        result.push_back(text.size());
      }
      return result;
    }
  }

  // Keep editing usable if Unicode initialization fails, while still never
  // placing a cursor inside a UTF-8 code unit sequence.
  std::vector<std::size_t> result{0};
  for (std::size_t offset = 0; offset < text.size();) {
    ++offset;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0U) == 0x80U) {
      ++offset;
    }
    result.push_back(offset);
  }
  return result;
}

} // namespace text
