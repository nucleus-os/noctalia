#include "render/core/image_file_loader.h"

#include <cstdio>
#include <cstdint>
#include <string>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "image_file_loader_data_uri_test: %s\n", message);
    }
    return condition;
  }

  bool checkPixel(
      const LoadedImageFile& image, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b,
      std::uint8_t a, const char* message
  ) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return check(false, message);
    const std::size_t offset = static_cast<std::size_t>(y * image.width + x) * 4U;
    const bool matches = image.rgba[offset] == r && image.rgba[offset + 1] == g
        && image.rgba[offset + 2] == b && image.rgba[offset + 3] == a;
    if (!matches) {
      std::fprintf(
          stderr, "image_file_loader_data_uri_test: %s: got rgba(%u,%u,%u,%u), expected rgba(%u,%u,%u,%u)\n",
          message, image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2], image.rgba[offset + 3],
          r, g, b, a
      );
    }
    return matches;
  }

} // namespace

int main() {
  bool ok = true;

  // A 1x1 PNG deliberately declared as image/jpeg. The loader should trust the
  // decoded bytes, not the data URI media type.
  const std::string mismatchedDataUri =
      "data:image/jpeg;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII=";

  auto image = loadImageFile(mismatchedDataUri);
  ok = check(image.has_value(), image ? "failed to decode mismatched data URI" : image.error().c_str()) && ok;
  if (image) {
    ok = check(image->width == 1, "decoded data URI width should be 1") && ok;
    ok = check(image->height == 1, "decoded data URI height should be 1") && ok;
    ok = check(image->rgba.size() == 4, "decoded data URI should contain one RGBA pixel") && ok;
  }

  const std::string pngDeclaredJpegDataUri =
      "data:image/png;base64,"
      "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJDRENDg8Q"
      "EBEQCgwSExIQEw8QEBD/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQ"
      "EAAD8AVN//2Q==";

  image = loadImageFile(pngDeclaredJpegDataUri);
  ok = check(image.has_value(), image ? "failed to decode PNG-declared JPEG data URI" : image.error().c_str()) && ok;
  if (image) {
    ok = check(image->width == 1, "decoded JPEG data URI width should be 1") && ok;
    ok = check(image->height == 1, "decoded JPEG data URI height should be 1") && ok;
    ok = check(image->rgba.size() == 4, "decoded JPEG data URI should contain one RGBA pixel") && ok;
  }

  image = loadImageFile("data:image/png;base64,not_base64!");
  ok = check(!image, "invalid base64 data URI should fail") && ok;
  if (!image) {
    const bool mentionsBase64 = image.error().find("base64") != std::string::npos;
    ok = check(mentionsBase64, "invalid base64 failure should explain the data issue") && ok;
  }

  image = loadImageFile("data:image/png;base64");
  ok = check(!image, "data URI without comma should fail") && ok;
  if (!image) {
    const bool mentionsSeparator = image.error().find("separator") != std::string::npos;
    ok = check(mentionsSeparator, "missing comma failure should mention separator") && ok;
  }

  // Pixel golden for the SkSVG path. Integer-aligned rectangles avoid
  // antialiasing ambiguity while covering orientation, transparent clear,
  // alpha un-premultiplication, intrinsic sizing, and CSS color parsing.
  const std::string svg =
      "data:image/svg+xml,"
      "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4' viewBox='0 0 4 4'>"
      "<rect x='0' y='0' width='2' height='2' fill='red'/>"
      "<rect x='2' y='0' width='2' height='2' fill='blue' fill-opacity='0.5'/>"
      "<rect x='0' y='2' width='2' height='2' fill='lime'/>"
      "</svg>";
  image = loadImageFile(svg);
  ok = check(image.has_value(), image ? "failed to rasterize SVG golden" : image.error().c_str()) && ok;
  if (image) {
    ok = check(image->width == 4 && image->height == 4, "SVG intrinsic dimensions should be preserved") && ok;
    ok = checkPixel(*image, 0, 0, 255, 0, 0, 255, "SVG top-left red pixel drifted") && ok;
    ok = checkPixel(*image, 3, 0, 0, 0, 255, 128, "SVG translucent blue pixel was not straight RGBA") && ok;
    ok = checkPixel(*image, 0, 3, 0, 255, 0, 255, "SVG Y orientation is inverted") && ok;
    ok = checkPixel(*image, 3, 3, 0, 0, 0, 0, "SVG transparent clear pixel drifted") && ok;
  }

  image = loadImageFile(svg, 8);
  ok = check(image.has_value(), image ? "failed to scale SVG golden" : image.error().c_str()) && ok;
  if (image) {
    ok = check(image->width == 8 && image->height == 8, "SVG target size should scale the container") && ok;
    ok = checkPixel(*image, 1, 1, 255, 0, 0, 255, "scaled SVG red region drifted") && ok;
    ok = checkPixel(*image, 7, 1, 0, 0, 255, 128, "scaled SVG alpha/color drifted") && ok;
    ok = checkPixel(*image, 7, 7, 0, 0, 0, 0, "scaled SVG transparent region drifted") && ok;
  }

  return ok ? 0 : 1;
}
