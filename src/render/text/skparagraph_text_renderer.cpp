#include "render/text/skparagraph_text_renderer.h"

#include "render/backend/render_backend.h"
#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/text/font_registry.h"

#include <nucleus/text/TextLayoutBuilder.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace nt = nucleus::text;

namespace {
  nt::TextAlignment alignment(TextAlign value) {
    if (value == TextAlign::Center) return nt::TextAlignment::Center;
    if (value == TextAlign::End) return nt::TextAlignment::Trailing;
    return nt::TextAlignment::Leading;
  }

  nt::EllipsisMode ellipsis(TextEllipsize value) {
    if (value == TextEllipsize::None) return nt::EllipsisMode::None;
    if (value == TextEllipsize::Start) return nt::EllipsisMode::Start;
    if (value == TextEllipsize::Middle) return nt::EllipsisMode::Middle;
    return nt::EllipsisMode::End;
  }

  nt::ParagraphDirection paragraphDirection(ParagraphDirection value) {
    if (value == ParagraphDirection::Ltr) return nt::ParagraphDirection::Ltr;
    if (value == ParagraphDirection::Rtl) return nt::ParagraphDirection::Rtl;
    return nt::ParagraphDirection::Automatic;
  }

  std::uint32_t utf16Offset(std::string_view text, std::size_t byteOffset) {
    byteOffset = std::min(byteOffset, text.size());
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < byteOffset;) {
      const auto c = static_cast<unsigned char>(text[i]);
      std::size_t count = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      if (i + count > byteOffset) break;
      result += count == 4 ? 2U : 1U;
      i += count;
    }
    return result;
  }

  struct ParsedRun {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool monospace = false;
    bool underline = false;
    bool strike = false;
    std::optional<Color> color;

  };

  std::vector<ParsedRun> parsedRuns(const std::vector<StyledTextRun>& runs, bool preserveColors = true) {
    std::vector<ParsedRun> parsed;
    parsed.reserve(runs.size());
    for (const auto& run : runs) {
      if (run.text.empty()) continue;
      parsed.push_back({
          .text = run.text,
          .bold = run.bold,
          .italic = run.italic,
          .monospace = run.monospace,
          .underline = run.underline,
          .strike = run.strikeThrough,
          .color = preserveColors ? run.color : std::nullopt,
      });
    }
    return parsed;
  }

}

struct SkParagraphTextRenderer::Impl {
  struct Entry {
    std::uint64_t handle = 0;
    TextMetrics metrics;
    float baseline = 0.0f;
    std::string text;
    std::list<std::string>::iterator lru;
  };

  RenderBackend* backend = nullptr;
  std::string family = "sans-serif";
  nt::TextLayoutService service;
  std::unordered_map<std::string, Entry> cache;
  std::unordered_map<std::uint64_t, TextMetrics> fontMetricsCache;
  std::list<std::string> lru;
  std::uint64_t observedFontGeneration = text::fontConfigGeneration();

  ~Impl() { clear(); }
  void clear() {
    for (auto& [key, entry] : cache) if (entry.handle != 0) service.release(entry.handle);
    cache.clear();
    lru.clear();
    fontMetricsCache.clear();
  }

  std::string key(std::string_view source, float size, FontWeight weight, float width, int lines,
                  TextAlign align, std::string_view font, TextEllipsize ellipsizeMode,
                  ParagraphDirection direction, Color color) const {
    const auto channel = [](float value) { return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
    const std::uint32_t rgba8 = (channel(color.r) << 24U) | (channel(color.g) << 16U) | (channel(color.b) << 8U) | channel(color.a);
    return std::string(source) + '\x1f' + std::string(font) + '\x1f'
        + std::to_string(std::lround(size * 64)) + ':'
        + std::to_string(static_cast<int>(weight)) + ':' + std::to_string(std::lround(width * 64)) + ':'
        + std::to_string(lines) + ':' + std::to_string(static_cast<int>(align)) + ':'
        + std::to_string(static_cast<int>(ellipsizeMode)) + ':' + std::to_string(static_cast<int>(direction)) + ':'
        + std::to_string(rgba8);
  }

  Entry* get(std::string_view source, float size, FontWeight weight, float width, int lines, TextAlign align,
             std::string_view requestedFamily, TextEllipsize ellipsizeMode, Color color,
             ParagraphDirection direction = ParagraphDirection::Automatic,
             const std::vector<ParsedRun>* styled = nullptr) {
    const std::uint64_t fontGeneration = text::fontConfigGeneration();
    if (fontGeneration != observedFontGeneration) {
      clear();
      observedFontGeneration = fontGeneration;
    }
    std::vector<ParsedRun> parsed =
        styled != nullptr ? *styled : std::vector<ParsedRun>{{std::string(source)}};
    std::string text; for (const auto& run : parsed) text += run.text;
    const std::string_view selectedFamily = requestedFamily.empty() ? std::string_view(family) : requestedFamily;
    static constexpr std::string_view monospaceFamily = "monospace";
    const auto makeViews = [&](const std::vector<ParsedRun>& segments) {
      std::vector<nt::TextRunView> views;
      views.reserve(segments.size());
      for (const auto& segment : segments) {
        const auto runFamily = segment.monospace ? monospaceFamily : selectedFamily;
        const Color runColor = segment.color.value_or(color);
        views.push_back(nt::TextRunView{
            .text={segment.text.data(),segment.text.size()}, .fontFamily={runFamily.data(),runFamily.size()},
            .pointSize=size, .weight=static_cast<std::uint32_t>(segment.bold?FontWeight::Bold:weight),
            .slant=segment.italic?nt::FontSlantItalic:nt::FontSlantUpright,
            .underline=segment.underline, .strikeThrough=segment.strike,
            .red=runColor.r,.green=runColor.g,.blue=runColor.b,
            .alpha=segment.color.has_value() ? runColor.a * color.a : runColor.a});
      }
      return views;
    };
    std::string cacheSource(source);
    if (styled != nullptr) {
      cacheSource += "\x1estructured:";
      for (const auto& run : parsed) {
        cacheSource += run.text;
        cacheSource.push_back('\x1d');
        cacheSource += std::to_string(run.bold) + std::to_string(run.italic) + std::to_string(run.monospace)
            + std::to_string(run.underline) + std::to_string(run.strike);
        if (run.color.has_value()) {
          cacheSource += ':' + std::to_string(run.color->r) + ':' + std::to_string(run.color->g) + ':'
              + std::to_string(run.color->b) + ':' + std::to_string(run.color->a);
        }
      }
    }
    std::string cacheKey = key(
        cacheSource, size, weight, width, lines, align, selectedFamily, ellipsizeMode, direction, color);
    if (auto it = cache.find(cacheKey); it != cache.end()) {
      lru.splice(lru.begin(), lru, it->second.lru);
      return &it->second;
    }
    auto runs = makeViews(parsed);
    nt::ParagraphStyle style{
        .width = width, .maximumNumberOfLines = static_cast<std::uint32_t>(std::max(0, lines)),
        .alignment = alignment(align), .ellipsisMode = ellipsis(ellipsizeMode),
        .direction = paragraphDirection(direction)};
    nt::ParagraphMetrics paragraph{};
    std::uint64_t handle = 0;
    if (!service.createRuns(runs.data(), runs.size(), &style, &handle, &paragraph)) return nullptr;
    std::vector<nt::TextLineMetrics> lineMetrics(paragraph.lineCount);
    service.metrics(handle, lineMetrics.data(), lineMetrics.size(), &paragraph);
    TextMetrics metrics{};
    metrics.lineCount = static_cast<int>(paragraph.lineCount);
    metrics.top = -paragraph.alphabeticBaseline;
    metrics.bottom = paragraph.height - paragraph.alphabeticBaseline;
    for (const auto& line : lineMetrics) metrics.width = std::max(metrics.width, line.x + line.width);
    // SkParagraph's line metrics exclude the separately shaped ellipsis run.
    // Once a width-constrained paragraph truncates, its layout box—not the
    // surviving source glyph advances—is therefore the only safe reported
    // width. Returning the shorter line width lets parents shrink underneath
    // the painted ellipsis, which is visible as text escaping a pill/button.
    if (paragraph.didExceedMaximumLines && width > 0.0f) {
      metrics.width = width;
    }
    metrics.right = metrics.width;
    nt::TextBounds ink{};
    if (service.inkBounds(handle, &ink)) {
      metrics.inkLeft = ink.left;
      metrics.inkRight = ink.right;
      metrics.inkTop = ink.top - paragraph.alphabeticBaseline;
      metrics.inkBottom = ink.bottom - paragraph.alphabeticBaseline;
    }
    lru.push_front(cacheKey);
    auto [it, inserted] = cache.emplace(std::move(cacheKey), Entry{handle, metrics, paragraph.alphabeticBaseline, text, lru.begin()});
    while (cache.size() > 1024) { auto old = cache.find(lru.back()); service.release(old->second.handle); cache.erase(old); lru.pop_back(); }
    return &it->second;
  }
};

SkParagraphTextRenderer::SkParagraphTextRenderer() : m_impl(std::make_unique<Impl>()) {}
SkParagraphTextRenderer::~SkParagraphTextRenderer() = default;
void SkParagraphTextRenderer::initialize(RenderBackend* backend) { m_impl->backend = backend; }
void SkParagraphTextRenderer::cleanup() { m_impl->clear(); m_impl->backend = nullptr; }
void SkParagraphTextRenderer::setContentScale(float) {}
void SkParagraphTextRenderer::setFontFamily(std::string family) { if (m_impl->family != family) { m_impl->family = std::move(family); m_impl->clear(); } }
void SkParagraphTextRenderer::notifyFontConfigChanged() {
  m_impl->clear();
  m_impl->service.invalidateFontCollection();
  m_impl->observedFontGeneration = text::fontConfigGeneration();
}

SkParagraphTextRenderer::TextMetrics SkParagraphTextRenderer::measure(std::string_view text, float size, FontWeight weight,
    float width, int lines, TextAlign align, std::string_view family, TextEllipsize mode, ParagraphDirection direction) {
  if (text.empty()) return {};
  auto* entry = m_impl->get(text, size, weight, width, lines, align, family, mode, rgba(1,1,1,1), direction);
  return entry ? entry->metrics : TextMetrics{};
}

SkParagraphTextRenderer::TextMetrics SkParagraphTextRenderer::measureStyled(
    const std::vector<StyledTextRun>& runs, float size, FontWeight weight, float width, int lines, TextAlign align,
    std::string_view family, TextEllipsize mode, ParagraphDirection direction) {
  const auto parsed = parsedRuns(runs);
  std::string text;
  for (const auto& run : parsed) text += run.text;
  if (text.empty()) return {};
  auto* entry = m_impl->get(
      text, size, weight, width, lines, align, family, mode, rgba(1, 1, 1, 1), direction, &parsed);
  return entry ? entry->metrics : TextMetrics{};
}

SkParagraphTextRenderer::TextMetrics SkParagraphTextRenderer::measureFont(float size, FontWeight weight) const {
  const auto sizeQ = static_cast<std::uint32_t>(std::lround(std::max(0.0f, size) * 64.0f));
  const std::uint64_t key = (static_cast<std::uint64_t>(sizeQ) << 32U)
      | static_cast<std::uint32_t>(weight);
  if (const auto it = m_impl->fontMetricsCache.find(key); it != m_impl->fontMetricsCache.end()) {
    return it->second;
  }
  nt::FontMetrics font{};
  const auto family = std::string_view(m_impl->family);
  if (!m_impl->service.queryFontMetrics({family.data(), family.size()}, size, static_cast<std::uint32_t>(weight),
      nt::FontWidthStandard, nt::FontSlantUpright, &font)) return {};
  const TextMetrics result{
      .top = -font.ascender, .bottom = font.descender, .inkTop = -font.ascender,
      .inkBottom = font.descender, .capHeight = font.capHeight};
  m_impl->fontMetricsCache.emplace(key, result);
  return result;
}

void SkParagraphTextRenderer::measureCursorStops(std::string_view text, float size,
    const std::vector<std::size_t>& offsets, std::vector<float>& out, FontWeight weight) {
  std::vector<TextCursorStop> wrapped;
  measureCursorStopsWrapped(text, size, offsets, 0.0f, wrapped, weight);
  out.resize(wrapped.size());
  for (std::size_t i = 0; i < wrapped.size(); ++i) out[i] = wrapped[i].x;
}

void SkParagraphTextRenderer::measureCursorStopsWrapped(std::string_view text, float size,
    const std::vector<std::size_t>& offsets, float width, std::vector<TextCursorStop>& out, FontWeight weight) {
  out.assign(offsets.size(), {});
  auto* entry = m_impl->get(text, size, weight, width, 0, TextAlign::Start, {}, TextEllipsize::End, rgba(1,1,1,1));
  if (!entry) return;
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto start = utf16Offset(entry->text, offsets[i]);
    nt::TextCaret downstream{};
    nt::TextCaret upstream{};
    const bool hasDownstream = m_impl->service.caretForOffset(
        entry->handle, start, nt::TextAffinityDownstream, &downstream
    );
    const bool hasUpstream = m_impl->service.caretForOffset(
        entry->handle, start, nt::TextAffinityUpstream, &upstream
    );
    const nt::TextCaret* primary = hasDownstream ? &downstream : (hasUpstream ? &upstream : nullptr);
    if (primary != nullptr) {
      out[i] = {.x = primary->x, .y = primary->y, .height = primary->height, .trailingX = primary->x};
      const nt::TextCaret* alternate = hasDownstream && hasUpstream ? &upstream : nullptr;
      if (alternate != nullptr
          && (std::abs(alternate->x - primary->x) > 0.5f || std::abs(alternate->y - primary->y) > 0.5f)) {
        out[i].alternateX = alternate->x;
        out[i].alternateY = alternate->y;
        out[i].alternateHeight = alternate->height;
        out[i].alternateValid = true;
      }
    }
    const std::size_t nextByte = i + 1 < offsets.size() ? offsets[i + 1] : entry->text.size();
    const auto end = utf16Offset(entry->text, std::max(offsets[i], nextByte));
    std::uint32_t count = 0;
    m_impl->service.rectsForRange(entry->handle, start, end, nullptr, 0, &count);
    nt::TextRect rect{};
    if (count && m_impl->service.rectsForRange(entry->handle, start, end, &rect, 1, &count)) {
      const float trailingEdge = rect.direction == nt::TextDirectionRtl ? rect.x : rect.x + rect.width;
      if (primary == nullptr) {
        out[i].x = rect.direction == nt::TextDirectionRtl ? rect.x + rect.width : rect.x;
        out[i].y = rect.y;
        out[i].height = rect.height;
      }
      out[i].trailingX = trailingEdge;
      out[i].rangeValid = true;
      continue;
    }
    if (primary != nullptr) continue;
    std::vector<nt::TextLineMetrics> lines(static_cast<std::size_t>(std::max(0, entry->metrics.lineCount)));
    nt::ParagraphMetrics paragraph{};
    if (m_impl->service.metrics(entry->handle, lines.data(), lines.size(), &paragraph) && !lines.empty()) {
      const auto& line = lines.back();
      out[i] = {
          .x = line.x + line.width, .y = line.y, .height = line.height,
          .trailingX = line.x + line.width,
      };
    } else {
      out[i] = {
          .x = entry->metrics.width, .y = 0, .height = entry->metrics.bottom - entry->metrics.top,
          .trailingX = entry->metrics.width,
      };
    }
  }
}

void SkParagraphTextRenderer::draw(float, float, float x, float baselineY, std::string_view text, float size,
    const Color& color, const Mat3& transform, FontWeight weight, float width, int lines, TextAlign align,
    std::string_view family, TextEllipsize mode, ParagraphDirection direction) {
  auto* entry = m_impl->get(text, size, weight, width, lines, align, family, mode, color, direction);
  if (entry && m_impl->backend) m_impl->backend->drawParagraph(entry->handle, x, baselineY - entry->baseline, transform);
}

void SkParagraphTextRenderer::drawStyled(
    float, float, float x, float baselineY, const std::vector<StyledTextRun>& runs, float size, const Color& color,
    const Mat3& transform, FontWeight weight, float width, int lines, TextAlign align, std::string_view family,
    TextEllipsize mode, ParagraphDirection direction, bool preserveRunColors) {
  const auto parsed = parsedRuns(runs, preserveRunColors);
  std::string text;
  for (const auto& run : parsed) text += run.text;
  auto* entry = m_impl->get(text, size, weight, width, lines, align, family, mode, color, direction, &parsed);
  if (entry && m_impl->backend) m_impl->backend->drawParagraph(entry->handle, x, baselineY - entry->baseline, transform);
}
