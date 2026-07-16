#pragma once

#include "render/core/renderer.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class RenderBackend;
struct Color;
struct Mat3;

class SkParagraphTextRenderer {
public:
  using TextMetrics = ::TextMetrics;

  SkParagraphTextRenderer();
  ~SkParagraphTextRenderer();
  SkParagraphTextRenderer(const SkParagraphTextRenderer&) = delete;
  SkParagraphTextRenderer& operator=(const SkParagraphTextRenderer&) = delete;

  void initialize(RenderBackend* backend);
  void cleanup();
  void setContentScale(float scale);
  void setFontFamily(std::string family);
  void notifyFontConfigChanged();
  void invalidateGlyphTextures() {}

  [[nodiscard]] TextMetrics measure(
      std::string_view text, float fontSize, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {},
      TextEllipsize ellipsize = TextEllipsize::End,
      ParagraphDirection direction = ParagraphDirection::Automatic);
  [[nodiscard]] TextMetrics measureStyled(
      const std::vector<StyledTextRun>& runs, float fontSize, FontWeight fontWeight = FontWeight::Normal,
      float maxWidth = 0.0f, int maxLines = 0, TextAlign align = TextAlign::Start,
      std::string_view fontFamily = {}, TextEllipsize ellipsize = TextEllipsize::End,
      ParagraphDirection direction = ParagraphDirection::Automatic);
  [[nodiscard]] TextMetrics measureFont(float fontSize, FontWeight fontWeight) const;
  void measureCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      FontWeight fontWeight = FontWeight::Normal);
  void measureCursorStopsWrapped(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, FontWeight fontWeight = FontWeight::Normal);
  void draw(
      float surfaceWidth, float surfaceHeight, float x, float baselineY, std::string_view text, float fontSize,
      const Color& color, const Mat3& transform, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {},
      TextEllipsize ellipsize = TextEllipsize::End,
      ParagraphDirection direction = ParagraphDirection::Automatic);
  void drawStyled(
      float surfaceWidth, float surfaceHeight, float x, float baselineY, const std::vector<StyledTextRun>& runs,
      float fontSize, const Color& color, const Mat3& transform, FontWeight fontWeight = FontWeight::Normal,
      float maxWidth = 0.0f, int maxLines = 0, TextAlign align = TextAlign::Start,
      std::string_view fontFamily = {}, TextEllipsize ellipsize = TextEllipsize::End,
      ParagraphDirection direction = ParagraphDirection::Automatic, bool preserveRunColors = true);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
