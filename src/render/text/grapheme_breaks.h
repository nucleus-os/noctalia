#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace text {

// UTF-8 byte offsets at Unicode extended-grapheme boundaries, including 0 and
// text.size(). Uses the same SkUnicode service as paragraph layout so editing
// and shaping agree about indivisible user-perceived characters.
[[nodiscard]] std::vector<std::size_t> graphemeBreaks(std::string_view text);

} // namespace text
