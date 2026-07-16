#include <nucleus/text/TextLayoutBuilder.hpp>

#include "render/text/font_registry.h"
#include "render/text/grapheme_breaks.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

namespace nt = nucleus::text;

namespace {
bool checkDirection(nt::TextLayoutService& service, const std::string& text, std::uint32_t expected) {
  nt::TextRunView run{.text = {text.data(), text.size()}, .pointSize = 20.0f};
  nt::ParagraphStyle style{.width = 400.0f, .direction = nt::ParagraphDirection::Automatic};
  nt::ParagraphMetrics metrics{};
  std::uint64_t handle = 0;
  if (!service.createRuns(&run, 1, &style, &handle, &metrics)) return false;
  std::uint32_t count = 0;
  const bool counted = service.rectsForRange(handle, 0, 1, nullptr, 0, &count);
  std::vector<nt::TextRect> rects(count);
  const bool measured = counted && count > 0
      && service.rectsForRange(handle, 0, 1, rects.data(), rects.size(), &count);
  service.release(handle);
  return measured && rects.front().direction == expected;
}
} // namespace

int main() {
  nt::TextLayoutService service;
  bool ok = true;

  // Combining marks are neutral. Automatic direction must continue to the
  // first strong Arabic character instead of classifying every non-ASCII code
  // point as LTR.
  if (!checkDirection(service, "\xCC\x81\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1", nt::TextDirectionRtl)) {
    std::cerr << "automatic Arabic direction was not RTL\n";
    ok = false;
  }
  if (!checkDirection(service, "\xCC\x81Latin", nt::TextDirectionLtr)) {
    std::cerr << "automatic Latin direction was not LTR\n";
    ok = false;
  }

  // Selection geometry must preserve visual direction and split across
  // wrapped lines; Noctalia uses these boxes rather than estimating spans
  // from neighboring carets.
  const std::string mixedBidi = "abc \xD7\x90\xD7\x91\xD7\x92";
  nt::TextRunView bidiRun{.text = {mixedBidi.data(), mixedBidi.size()}, .pointSize = 20.0f};
  nt::ParagraphStyle bidiStyle{.width = 200.0f};
  nt::ParagraphMetrics bidiMetrics{};
  std::uint64_t bidiHandle = 0;
  if (!service.createRuns(&bidiRun, 1, &bidiStyle, &bidiHandle, &bidiMetrics)) {
    std::cerr << "failed to create mixed-bidi selection probe\n";
    ok = false;
  } else {
    std::uint32_t bidiRectCount = 0;
    service.rectsForRange(bidiHandle, 4, 7, nullptr, 0, &bidiRectCount);
    std::vector<nt::TextRect> bidiRects(bidiRectCount);
    if (bidiRectCount == 0
        || !service.rectsForRange(bidiHandle, 4, 7, bidiRects.data(), bidiRects.size(), &bidiRectCount)
        || bidiRects.front().direction != nt::TextDirectionRtl || bidiRects.front().width <= 0.0f) {
      std::cerr << "mixed-bidi selection lost its RTL range geometry\n";
      ok = false;
    }
    nt::TextCaret upstreamCaret{};
    nt::TextCaret downstreamCaret{};
    if (!service.caretForOffset(bidiHandle, 4, nt::TextAffinityUpstream, &upstreamCaret)
        || !service.caretForOffset(bidiHandle, 4, nt::TextAffinityDownstream, &downstreamCaret)
        || upstreamCaret.affinity != nt::TextAffinityUpstream
        || downstreamCaret.affinity != nt::TextAffinityDownstream
        || downstreamCaret.direction != nt::TextDirectionRtl
        || std::abs(upstreamCaret.x - downstreamCaret.x) < 1.0f) {
      std::cerr << "mixed-bidi boundary did not expose distinct caret affinities\n";
      ok = false;
    }
    service.release(bidiHandle);
  }

  constexpr std::string_view wrappedText = "one two three four five";
  nt::TextRunView wrappedRun{.text = {wrappedText.data(), wrappedText.size()}, .pointSize = 18.0f};
  nt::ParagraphStyle wrappedStyle{.width = 65.0f};
  nt::ParagraphMetrics wrappedMetrics{};
  std::uint64_t wrappedHandle = 0;
  if (!service.createRuns(&wrappedRun, 1, &wrappedStyle, &wrappedHandle, &wrappedMetrics)) {
    std::cerr << "failed to create wrapped selection probe\n";
    ok = false;
  } else {
    std::uint32_t wrappedRectCount = 0;
    service.rectsForRange(
        wrappedHandle, 0, static_cast<std::uint32_t>(wrappedText.size()), nullptr, 0, &wrappedRectCount
    );
    if (wrappedMetrics.lineCount < 2 || wrappedRectCount < 2) {
      std::cerr << "wrapped selection did not split into line geometry\n";
      ok = false;
    }
    service.release(wrappedHandle);
  }

  const std::string graphemes = "e\xCC\x81x \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
  std::uint32_t count = 0;
  if (!service.graphemeBreaks({graphemes.data(), graphemes.size()}, nullptr, 0, &count)) {
    std::cerr << "failed to count grapheme boundaries\n";
    return 1;
  }
  std::vector<std::uint32_t> breaks(count);
  if (!service.graphemeBreaks({graphemes.data(), graphemes.size()}, breaks.data(), breaks.size(), &count)
      || breaks.empty() || breaks.front() != 0 || breaks.back() != graphemes.size()
      || std::ranges::find(breaks, 1U) != breaks.end()) {
    std::cerr << "grapheme boundaries split a combining sequence or omitted endpoints\n";
    ok = false;
  }
  const auto editorBreaks = text::graphemeBreaks(graphemes);
  if (editorBreaks.size() != breaks.size()
      || !std::equal(editorBreaks.begin(), editorBreaks.end(), breaks.begin())) {
    std::cerr << "editor grapheme boundaries diverged from the shared text service\n";
    ok = false;
  }

  // Invalidation must leave the shared service immediately usable; plugin
  // font registration relies on this path to rebuild all font resolution.
  service.invalidateFontCollection();
  nt::FontMetrics font{};
  constexpr std::string_view family = "sans-serif";
  if (!service.queryFontMetrics(
          {family.data(), family.size()}, 16.0f, nt::FontWeightRegular, nt::FontWidthStandard,
          nt::FontSlantUpright, &font)
      || font.capHeight <= 0.0f) {
    std::cerr << "font collection did not recover after invalidation\n";
    ok = false;
  }

  constexpr std::string_view inkText = "Hgj";
  nt::TextRunView inkRun{.text = {inkText.data(), inkText.size()}, .pointSize = 28.0f};
  nt::ParagraphStyle inkStyle{.width = 160.0f};
  nt::ParagraphMetrics inkMetrics{};
  std::uint64_t inkHandle = 0;
  nt::TextBounds inkBounds{};
  if (!service.createRuns(&inkRun, 1, &inkStyle, &inkHandle, &inkMetrics)
      || !service.inkBounds(inkHandle, &inkBounds)
      || inkBounds.right <= inkBounds.left || inkBounds.bottom <= inkBounds.top
      || inkBounds.right > inkMetrics.width + 0.5f || inkBounds.bottom > inkMetrics.height + 0.5f) {
    std::cerr << "paragraph did not expose valid glyph ink bounds\n";
    ok = false;
  }
  if (inkHandle != 0) service.release(inkHandle);

  // Raster parity probe for the production paint API. It deliberately checks
  // color regions rather than a platform-font-dependent byte hash, while still
  // catching missing runs, lost run colors, an opaque clear, or a paint path
  // that does not target the supplied canvas.
  constexpr std::string_view left = "Noctalia ";
  constexpr std::string_view right = "Graphite";
  const nt::TextRunView paintedRuns[] = {
      {.text = {left.data(), left.size()}, .pointSize = 22.0f, .weight = nt::FontWeightBold,
       .underline = true, .red = 1.0f, .green = 0.0f, .blue = 0.0f, .alpha = 1.0f},
      {.text = {right.data(), right.size()}, .pointSize = 22.0f, .strikeThrough = true,
       .red = 0.0f, .green = 1.0f, .blue = 0.0f, .alpha = 1.0f},
  };
  nt::ParagraphStyle paintedStyle{.width = 256.0f};
  nt::ParagraphMetrics paintedMetrics{};
  std::uint64_t paintedHandle = 0;
  auto surface = SkSurfaces::Raster(
      SkImageInfo::Make(256, 64, kRGBA_8888_SkColorType, kPremul_SkAlphaType)
  );
  if (surface == nullptr
      || !service.createRuns(paintedRuns, 2, &paintedStyle, &paintedHandle, &paintedMetrics)) {
    std::cerr << "failed to create styled paragraph raster probe\n";
    ok = false;
  } else {
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    if (!service.paint(paintedHandle, surface->getCanvas(), 0.0f, 0.0f)) {
      std::cerr << "failed to paint styled paragraph\n";
      ok = false;
    } else {
      SkPixmap pixels;
      bool sawRed = false, sawGreen = false, sawTransparent = false;
      if (!surface->peekPixels(&pixels)) {
        std::cerr << "failed to read styled paragraph raster\n";
        ok = false;
      } else {
        for (int y = 0; y < pixels.height(); ++y) {
          for (int x = 0; x < pixels.width(); ++x) {
            const SkColor color = pixels.getColor(x, y);
            sawRed |= SkColorGetA(color) > 0 && SkColorGetR(color) > SkColorGetG(color) * 2;
            sawGreen |= SkColorGetA(color) > 0 && SkColorGetG(color) > SkColorGetR(color) * 2;
            sawTransparent |= SkColorGetA(color) == 0;
          }
        }
        if (!sawRed || !sawGreen || !sawTransparent) {
          std::cerr << "styled paragraph raster lost run colors or transparent background\n";
          ok = false;
        }
      }
    }
    service.release(paintedHandle);
  }

  const auto ellipsisColors = [&](nt::EllipsisMode mode) {
    constexpr std::string_view prefix = "AAAAAAAAAA";
    constexpr std::string_view suffix = "BBBBBBBBBB";
    const nt::TextRunView runs[] = {
        {.text = {prefix.data(), prefix.size()}, .pointSize = 22.0f,
         .red = 1.0f, .green = 0.0f, .blue = 0.0f, .alpha = 1.0f},
        {.text = {suffix.data(), suffix.size()}, .pointSize = 22.0f,
         .red = 0.0f, .green = 1.0f, .blue = 0.0f, .alpha = 1.0f},
    };
    nt::ParagraphStyle style{
        .width = 110.0f, .maximumNumberOfLines = 1, .ellipsisMode = mode,
    };
    nt::ParagraphMetrics metrics{};
    std::uint64_t handle = 0;
    auto target = SkSurfaces::Raster(
        SkImageInfo::Make(128, 48, kRGBA_8888_SkColorType, kPremul_SkAlphaType)
    );
    bool red = false, green = false;
    if (target != nullptr && service.createRuns(runs, 2, &style, &handle, &metrics)) {
      target->getCanvas()->clear(SK_ColorTRANSPARENT);
      service.paint(handle, target->getCanvas(), 0.0f, 0.0f);
      SkPixmap pixels;
      if (target->peekPixels(&pixels)) {
        for (int y = 0; y < pixels.height(); ++y) {
          for (int x = 0; x < pixels.width(); ++x) {
            const SkColor color = pixels.getColor(x, y);
            red |= SkColorGetA(color) > 0 && SkColorGetR(color) > SkColorGetG(color) * 2;
            green |= SkColorGetA(color) > 0 && SkColorGetG(color) > SkColorGetR(color) * 2;
          }
        }
      }
      if (metrics.maxIntrinsicWidth > style.width + 0.5f || metrics.lineCount != 1) {
        std::cerr << "custom ellipsis exceeded its one-line width contract\n";
        ok = false;
      }
      service.release(handle);
    } else {
      std::cerr << "failed to create custom ellipsis paragraph\n";
      ok = false;
    }
    return std::pair{red, green};
  };

  const auto startColors = ellipsisColors(nt::EllipsisMode::Start);
  if (startColors.first || !startColors.second) {
    std::cerr << "start ellipsis did not retain only the styled suffix\n";
    ok = false;
  }
  const auto middleColors = ellipsisColors(nt::EllipsisMode::Middle);
  if (!middleColors.first || !middleColors.second) {
    std::cerr << "middle ellipsis did not preserve styles on both sides\n";
    ok = false;
  }

  // A ZWJ family and combining sequence must remain indivisible while the
  // shared service searches for a middle-ellipsis candidate.
  const std::string complex =
      "left-e\xCC\x81-\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7-right";
  nt::TextRunView complexRun{.text = {complex.data(), complex.size()}, .pointSize = 18.0f};
  nt::ParagraphStyle complexStyle{
      .width = 90.0f, .maximumNumberOfLines = 1, .ellipsisMode = nt::EllipsisMode::Middle,
  };
  nt::ParagraphMetrics complexMetrics{};
  if (!service.measureRuns(&complexRun, 1, &complexStyle, nullptr, 0, &complexMetrics)
      || complexMetrics.lineCount != 1 || complexMetrics.maxIntrinsicWidth > complexStyle.width + 0.5f) {
    std::cerr << "grapheme-safe middle ellipsis failed its width contract\n";
    ok = false;
  }

  // Prime the shared collection first, then add an application font. This is
  // the ordering that previously left plugin fonts invisible because the
  // generation counter was incremented but never consumed.
  const auto generationBefore = text::fontConfigGeneration();
  const std::filesystem::path tablerPath =
      std::filesystem::path(NOCTALIA_SOURCE_ASSETS_DIR) / "fonts" / "tabler.ttf";
  const std::string tablerFamily = text::registerFontFile(tablerPath);
  if (tablerFamily != "noctalia-tabler-icons"
      || text::fontConfigGeneration() != generationBefore + 1) {
    std::cerr << "plugin font registration did not publish its family/generation\n";
    ok = false;
  }
  const std::string secondFamily = text::registerFontFile(tablerPath);
  if (secondFamily != tablerFamily || text::fontConfigGeneration() != generationBefore + 1) {
    std::cerr << "plugin font registration was not idempotent\n";
    ok = false;
  }
  nt::ResolvedFontDescriptor resolved{};
  const bool resolvedPlugin = service.resolveFont(
          {tablerFamily.data(), tablerFamily.size()}, 24.0f, nt::FontWeightRegular,
          nt::FontWidthStandard, nt::FontSlantUpright, &resolved);
  if (!resolvedPlugin || std::string_view(resolved.familyName, resolved.familyNameLength) != tablerFamily) {
    std::cerr << "new plugin font was not visible through the rebuilt shared collection (requested="
              << tablerFamily << ", resolved="
              << std::string_view(resolved.familyName, resolved.familyNameLength) << ")\n";
    ok = false;
  }
  const std::string icon = "\xEE\xB0\xB6"; // U+EC36, tabler a-b
  nt::TextRunView iconRun{
      .text = {icon.data(), icon.size()},
      .fontFamily = {tablerFamily.data(), tablerFamily.size()},
      .pointSize = 28.0f,
  };
  nt::ParagraphStyle iconStyle{.width = 48.0f};
  nt::ParagraphMetrics iconMetrics{};
  std::uint64_t iconHandle = 0;
  auto iconSurface = SkSurfaces::Raster(
      SkImageInfo::Make(48, 48, kRGBA_8888_SkColorType, kPremul_SkAlphaType)
  );
  bool paintedIcon = false;
  if (iconSurface != nullptr && service.createRuns(&iconRun, 1, &iconStyle, &iconHandle, &iconMetrics)) {
    iconSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
    service.paint(iconHandle, iconSurface->getCanvas(), 0.0f, 0.0f);
    SkPixmap pixels;
    if (iconSurface->peekPixels(&pixels)) {
      for (int y = 0; y < pixels.height() && !paintedIcon; ++y) {
        for (int x = 0; x < pixels.width(); ++x) {
          paintedIcon |= SkColorGetA(pixels.getColor(x, y)) > 0;
        }
      }
    }
    service.release(iconHandle);
  }
  if (!paintedIcon || iconMetrics.maxIntrinsicWidth <= 0.0f) {
    std::cerr << "registered plugin font did not paint its requested glyph\n";
    ok = false;
  }

  // Exercise the color-glyph paint path when the acceptance font is present.
  // The distro/package matrix provisions Noto Color Emoji; local minimal test
  // environments may omit it, in which case font fallback is covered above.
  constexpr std::string_view emojiFamily = "Noto Color Emoji";
  nt::ResolvedFontDescriptor emojiFont{};
  if (service.resolveFont(
          {emojiFamily.data(), emojiFamily.size()}, 40.0f, nt::FontWeightRegular,
          nt::FontWidthStandard, nt::FontSlantUpright, &emojiFont)
      && std::string_view(emojiFont.familyName, emojiFont.familyNameLength).contains(emojiFamily)) {
    const std::string emoji = "\xF0\x9F\x8C\x88"; // rainbow
    nt::TextRunView emojiRun{
        .text = {emoji.data(), emoji.size()},
        .fontFamily = {emojiFamily.data(), emojiFamily.size()},
        .pointSize = 40.0f,
    };
    nt::ParagraphStyle emojiStyle{.width = 64.0f};
    nt::ParagraphMetrics emojiMetrics{};
    std::uint64_t emojiHandle = 0;
    auto emojiSurface = SkSurfaces::Raster(
        SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType, kPremul_SkAlphaType)
    );
    bool sawChromaticPixel = false;
    if (emojiSurface != nullptr
        && service.createRuns(&emojiRun, 1, &emojiStyle, &emojiHandle, &emojiMetrics)) {
      emojiSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
      service.paint(emojiHandle, emojiSurface->getCanvas(), 0.0f, 0.0f);
      SkPixmap pixels;
      if (emojiSurface->peekPixels(&pixels)) {
        for (int y = 0; y < pixels.height() && !sawChromaticPixel; ++y) {
          for (int x = 0; x < pixels.width(); ++x) {
            const SkColor color = pixels.getColor(x, y);
            const int minimum = std::min({SkColorGetR(color), SkColorGetG(color), SkColorGetB(color)});
            const int maximum = std::max({SkColorGetR(color), SkColorGetG(color), SkColorGetB(color)});
            sawChromaticPixel |= SkColorGetA(color) > 0 && maximum - minimum > 24;
          }
        }
      }
      service.release(emojiHandle);
    }
    if (!sawChromaticPixel) {
      std::cerr << "installed color emoji font did not paint chromatic glyph pixels\n";
      ok = false;
    }
  }

  return ok ? 0 : 1;
}
