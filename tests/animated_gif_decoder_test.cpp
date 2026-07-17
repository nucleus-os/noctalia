#include "render/core/image_decoder.h"

#include <array>
#include <cstdio>

namespace {

  constexpr std::array<std::uint8_t, 109> kTwoFrameGif = {
      0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x01, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
      0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
      0x02, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x08, 0x05,
      0x00, 0x01, 0x00, 0x08, 0x08, 0x00, 0x21, 0xf9, 0x04, 0x05, 0x03, 0x00, 0x01, 0x00, 0x2c, 0x00,
      0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x08, 0x05, 0x00, 0x01, 0x00, 0x08, 0x08, 0x00, 0x3b,
  };

  bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "animated_gif_decoder_test: %s\n", message);
    return condition;
  }

  bool isSolid(const DecodedRasterFrame& frame, std::uint8_t red, std::uint8_t blue) {
    return frame.rgba.size() == 8
        && frame.rgba[0] == red
        && frame.rgba[1] == 0
        && frame.rgba[2] == blue
        && frame.rgba[3] == 255
        && frame.rgba[4] == red
        && frame.rgba[5] == 0
        && frame.rgba[6] == blue
        && frame.rgba[7] == 255;
  }

} // namespace

int main() {
  bool ok = true;
  auto decoded = decodeAnimatedGif(kTwoFrameGif.data(), kTwoFrameGif.size(), 8, 1024);
  ok = check(decoded.has_value(), decoded ? "" : decoded.error().c_str()) && ok;
  if (decoded) {
    ok = check(decoded->width == 2 && decoded->height == 1, "dimensions changed") && ok;
    ok = check(decoded->frames.size() == 2, "frame count changed") && ok;
    ok = check(!decoded->truncated, "complete animation was marked truncated") && ok;
    if (decoded->frames.size() == 2) {
      ok = check(isSolid(decoded->frames[0], 255, 0), "first frame is not solid red") && ok;
      ok = check(isSolid(decoded->frames[1], 0, 255), "second frame is not solid blue") && ok;
      ok = check(decoded->frames[0].durationMs == 20, "first duration changed") && ok;
      ok = check(decoded->frames[1].durationMs == 30, "second duration changed") && ok;
    }
  }

  auto capped = decodeAnimatedGif(kTwoFrameGif.data(), kTwoFrameGif.size(), 1, 1024);
  ok = check(capped && capped->frames.size() == 1 && capped->truncated, "frame cap was not enforced") && ok;
  return ok ? 0 : 1;
}
