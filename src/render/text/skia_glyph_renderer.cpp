#include "render/text/skia_glyph_renderer.h"

#include "render/backend/render_backend.h"
#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/text/font_registry.h"

#include <nucleus/text/TextLayoutBuilder.hpp>

#include <unordered_map>

namespace nt = nucleus::text;

namespace {
  std::string utf8(char32_t cp) {
    std::string s;
    if (cp <= 0x7f) s.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) { s.push_back(static_cast<char>(0xc0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
    else if (cp <= 0xffff) { s.push_back(static_cast<char>(0xe0 | (cp >> 12))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); s.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
    else { s.push_back(static_cast<char>(0xf0 | (cp >> 18))); s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); s.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
    return s;
  }
}

struct SkiaGlyphRenderer::Impl {
  struct Entry { std::uint64_t handle = 0; TextMetrics metrics; float baseline = 0; };
  std::string family;
  RenderBackend* backend = nullptr;
  nt::TextLayoutService service;
  std::unordered_map<std::uint64_t, Entry> cache;
  ~Impl() { for (auto& [key, e] : cache) service.release(e.handle); }
};

SkiaGlyphRenderer::SkiaGlyphRenderer() : m_impl(std::make_unique<Impl>()) {}
SkiaGlyphRenderer::~SkiaGlyphRenderer() = default;
void SkiaGlyphRenderer::initialize(const std::string& path, RenderBackend* backend) {
  m_impl->family = text::registerFontFile(path); m_impl->backend = backend;
}
void SkiaGlyphRenderer::cleanup() { for (auto& [key,e] : m_impl->cache) m_impl->service.release(e.handle); m_impl->cache.clear(); m_impl->backend = nullptr; }
SkiaGlyphRenderer::TextMetrics SkiaGlyphRenderer::measureGlyph(char32_t cp, float size) {
  const auto key = (static_cast<std::uint64_t>(cp) << 32U) | static_cast<std::uint32_t>(size * 64.0f + 0.5f);
  if (auto it = m_impl->cache.find(key); it != m_impl->cache.end()) return it->second.metrics;
  const std::string text = utf8(cp);
  nt::TextRunView run{.text={text.data(),text.size()}, .fontFamily={m_impl->family.data(),m_impl->family.size()}, .pointSize=size, .weight=400};
  nt::ParagraphStyle style{}; nt::ParagraphMetrics pm{}; std::uint64_t handle=0;
  if (!m_impl->service.createRuns(&run,1,&style,&handle,&pm)) return {};
  std::uint32_t count=0; m_impl->service.rectsForRange(handle,0,cp>0xffff?2:1,nullptr,0,&count);
  nt::TextRect rect{}; m_impl->service.rectsForRange(handle,0,cp>0xffff?2:1,&rect,1,&count);
  Impl::Entry entry{handle,{rect.width,rect.x,rect.x+rect.width,rect.y-pm.alphabeticBaseline,rect.y+rect.height-pm.alphabeticBaseline},pm.alphabeticBaseline};
  return m_impl->cache.emplace(key,entry).first->second.metrics;
}
void SkiaGlyphRenderer::drawGlyph(float,float,float x,float baselineY,char32_t cp,float size,const Color& color,const Mat3& transform) {
  const auto key=(static_cast<std::uint64_t>(cp)<<32U)|static_cast<std::uint32_t>(size*64.0f+0.5f); (void)measureGlyph(cp,size);
  if (auto it=m_impl->cache.find(key); it!=m_impl->cache.end() && m_impl->backend) m_impl->backend->drawTintedParagraph(it->second.handle,x,baselineY-it->second.baseline,color,transform);
}
