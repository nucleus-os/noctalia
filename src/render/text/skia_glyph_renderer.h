#pragma once

#include <memory>
#include <string>

class RenderBackend;
struct Color;
struct Mat3;

class SkiaGlyphRenderer {
public:
  struct TextMetrics { float width = 0, left = 0, right = 0, top = 0, bottom = 0; };
  SkiaGlyphRenderer();
  ~SkiaGlyphRenderer();
  SkiaGlyphRenderer(const SkiaGlyphRenderer&) = delete;
  SkiaGlyphRenderer& operator=(const SkiaGlyphRenderer&) = delete;
  void initialize(const std::string& fontPath, RenderBackend* backend);
  void cleanup();
  void setContentScale(float) {}
  void invalidateGlyphTextures() {}
  [[nodiscard]] TextMetrics measureGlyph(char32_t codepoint, float fontSize);
  void drawGlyph(float, float, float x, float baselineY, char32_t codepoint, float fontSize,
                 const Color& color, const Mat3& transform);
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
