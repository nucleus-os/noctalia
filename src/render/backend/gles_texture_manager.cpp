#include "render/backend/gles_texture_manager.h"

#include "core/log.h"
#include "render/core/image_decoder.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

  constexpr Logger kLog("texture");
  std::atomic<std::uint64_t> g_nextTextureGeneration{1};

#ifndef GL_BGRA_EXT
  constexpr GLenum GL_BGRA_EXT = 0x80E1;
#endif

  // DRM FourCC helpers (avoid a hard dependency on <drm_fourcc.h>).
  constexpr std::uint32_t drmFourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8)
        | (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
  }
  constexpr std::uint64_t kDrmModifierInvalid = 0x00ffffffffffffffULL;

  [[nodiscard]] GLuint toGlesTexture(TextureId id) noexcept { return static_cast<GLuint>(id.value()); }

  [[nodiscard]] GLenum toGlesFormat(TextureDataFormat format) noexcept {
    switch (format) {
    case TextureDataFormat::Alpha:
      return GL_ALPHA;
    case TextureDataFormat::LuminanceAlpha:
      return GL_LUMINANCE_ALPHA;
    case TextureDataFormat::Rgba:
      return GL_RGBA;
    }
    return GL_RGBA;
  }

  [[nodiscard]] GLint toGlesFilter(TextureFilter filter) noexcept {
    return filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
  }

  [[nodiscard]] GLint toGlesMipmapFilter(TextureFilter filter) noexcept {
    return filter == TextureFilter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
  }

  // Drops errors raised before the call we are about to check. Bounded because a robust context that
  // has been lost keeps reporting GL_CONTEXT_LOST, which would hang an unbounded drain.
  void clearGlErrors() {
    constexpr int kMaxDrain = 32;
    for (int i = 0; i < kMaxDrain && glGetError() != GL_NO_ERROR; ++i) {
    }
  }

  // Uploads level 0 and reports whether the driver accepted it. GLES2 requires internal format and
  // format to be equal, so both come from `format`.
  [[nodiscard]] bool
  texImage2dChecked(GLenum format, int width, int height, const std::uint8_t* data, const char* what) {
    clearGlErrors();
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0, format, GL_UNSIGNED_BYTE, data);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
      kLog.warn("glTexImage2D failed for {}x{} {} texture (format=0x{:x}): 0x{:x}", width, height, what, format, err);
      return false;
    }
    return true;
  }

} // namespace

GlesTextureManager::GlesTextureManager() : m_generation(g_nextTextureGeneration.fetch_add(1)) {}

TextureHandle GlesTextureManager::decodeEncodedRaster(
    const std::uint8_t* data, std::size_t size, const std::string* debugPath, bool mipmap
) {
  if (data == nullptr || size == 0) {
    return {};
  }

  if (auto decoded = decodeRasterImage(data, size)) {
    return uploadRgba(decoded->pixels.data(), decoded->width, decoded->height, mipmap);
  } else if (debugPath != nullptr) {
    kLog.warn("failed to decode image: {} ({})", ImageSourceLog::describe(*debugPath), decoded.error());
  }

  return {};
}

GlesTextureManager::~GlesTextureManager() { cleanup(); }

TextureHandle GlesTextureManager::loadFromFile(const std::string& path, int targetSize, bool mipmap) {
  auto loaded = loadImageFile(path, targetSize);
  if (!loaded) {
    kLog.warn("failed to load image: {} ({})", ImageSourceLog::describe(path), loaded.error());
    return {};
  }

  return loadFromRgba(loaded->rgba.data(), loaded->width, loaded->height, mipmap);
}

TextureHandle GlesTextureManager::loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap) {
  return decodeEncodedRaster(data, size, nullptr, mipmap);
}

TextureHandle GlesTextureManager::loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return {};
  }
  return uploadRgba(data, width, height, mipmap);
}

TextureHandle GlesTextureManager::loadFromPixels(
    const std::uint8_t* data, int width, int height, TextureDataFormat format, TextureFilter filter, bool mipmap
) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return {};
  }
  return uploadPixels(data, width, height, format, filter, mipmap);
}

TextureHandle GlesTextureManager::createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter) {
  if (width <= 0 || height <= 0) {
    return {};
  }
  return uploadPixels(nullptr, width, height, format, filter, false);
}

TextureHandle GlesTextureManager::createBgraSurface(int width, int height) {
  if (width <= 0 || height <= 0) {
    return {};
  }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (tex == 0) {
    kLog.warn("glGenTextures failed for {}x{} BGRA surface", width, height);
    return {};
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  // Allocate storage once. Where GL_BGRA_EXT is available the texel format
  // matches the source, so per-frame updates are a straight upload; otherwise
  // allocate RGBA and swizzle on update.
  const GLenum fmt = m_hasBgraExt ? GL_BGRA_EXT : GL_RGBA;
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(fmt), width, height, 0, fmt, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  m_textures.emplace_back(tex);
  return TextureHandle{.id = TextureId{tex}, .width = width, .height = height};
}

bool GlesTextureManager::updateBgraSurface(TextureHandle& handle, const std::uint8_t* data, int width, int height) {
  if (handle.id == 0 || data == nullptr || width != handle.width || height != handle.height) {
    return false;
  }
  glBindTexture(GL_TEXTURE_2D, toGlesTexture(handle.id));
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (m_hasBgraExt) {
    // Zero-copy in-place upload — no realloc, no swizzle.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
    return true;
  }
  // Fallback (no BGRA extension): swizzle once into a reused RGBA scratch.
  const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  m_bgraFallbackScratch.resize(pixels * 4U);
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t s = i * 4U;
    m_bgraFallbackScratch[s + 0] = data[s + 2];
    m_bgraFallbackScratch[s + 1] = data[s + 1];
    m_bgraFallbackScratch[s + 2] = data[s + 0];
    m_bgraFallbackScratch[s + 3] = data[s + 3];
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, m_bgraFallbackScratch.data());
  return true;
}

TextureHandle GlesTextureManager::loadFromRaw(
    const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format, bool mipmap
) {
  if (data == nullptr || size == 0 || width <= 0 || height <= 0) {
    return {};
  }

  const std::size_t channels = (format == PixmapFormat::RGB || format == PixmapFormat::BGR) ? 3U : 4U;
  const auto widthSize = static_cast<std::size_t>(width);
  const auto heightSize = static_cast<std::size_t>(height);
  const std::size_t minStride = widthSize * channels;
  const std::size_t actualStride = stride > 0 ? static_cast<std::size_t>(stride) : minStride;
  if (actualStride < minStride) {
    kLog.warn("raw pixmap stride too small: width={} channels={} stride={}", width, channels, stride);
    return {};
  }

  const std::size_t requiredSize = (heightSize - 1U) * actualStride + minStride;
  if (size < requiredSize) {
    kLog.warn(
        "raw pixmap buffer too small: width={} height={} stride={} have={} need={}", width, height, stride, size,
        requiredSize
    );
    return {};
  }

  const std::size_t widthBytes4 = static_cast<std::size_t>(width) * 4U;

  if (format == PixmapFormat::RGBA && actualStride == widthBytes4) {
    return uploadRgba(data, width, height, mipmap);
  }

  if (format == PixmapFormat::BGRA && m_hasBgraExt) {
    if (actualStride == widthBytes4) {
      return uploadBgra(data, width, height, mipmap);
    }
    std::vector<std::uint8_t> tight(widthBytes4 * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
      const auto row = static_cast<std::size_t>(y);
      std::memcpy(tight.data() + row * widthBytes4, data + row * actualStride, widthBytes4);
    }
    return uploadBgra(tight.data(), width, height, mipmap);
  }

  if (format == PixmapFormat::RGBA) {
    std::vector<std::uint8_t> tight(widthBytes4 * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
      const auto row = static_cast<std::size_t>(y);
      std::memcpy(tight.data() + row * widthBytes4, data + row * actualStride, widthBytes4);
    }
    return uploadRgba(tight.data(), width, height, mipmap);
  }

  const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::vector<std::uint8_t> rgba(pixelCount * 4);

  for (int y = 0; y < height; ++y) {
    const auto row = static_cast<std::size_t>(y);
    const std::uint8_t* srcRow = data + row * actualStride;
    std::uint8_t* dstRow = rgba.data() + row * widthSize * 4U;

    for (int x = 0; x < width; ++x) {
      const std::uint8_t* s = srcRow + (static_cast<std::size_t>(x) * channels);
      std::uint8_t* d = dstRow + static_cast<std::size_t>(x) * 4U;

      switch (format) {
      case PixmapFormat::BGRA:
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
        break;
      case PixmapFormat::ARGB:
        d[0] = s[1];
        d[1] = s[2];
        d[2] = s[3];
        d[3] = s[0];
        break;
      case PixmapFormat::RGB:
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
        break;
      case PixmapFormat::BGR:
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
        break;
      default:
        break;
      }
    }
  }

  return uploadRgba(rgba.data(), width, height, mipmap);
}

void GlesTextureManager::unload(TextureHandle& handle) {
  if (handle.id != 0) {
    const auto it = std::ranges::find(m_textures, handle.id);
    if (handle.generation == m_generation && it != m_textures.end()) {
      GLuint texture = toGlesTexture(handle.id);
      if (auto image = m_eglImages.find(texture); image != m_eglImages.end()) {
        if (m_eglDestroyImage != nullptr && image->second != EGL_NO_IMAGE_KHR) {
          m_eglDestroyImage(m_eglDisplay, image->second);
        }
        m_eglImages.erase(image);
      }
      glDeleteTextures(1, &texture);
      m_textures.erase(it);
    }
    handle = {};
  }
}

bool GlesTextureManager::replace(
    TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
    TextureFilter filter, bool mipmap
) {
  TextureHandle next = loadFromPixels(data, width, height, format, filter, mipmap);
  if (next.id == 0) {
    return false;
  }
  unload(handle);
  handle = next;
  return true;
}

bool GlesTextureManager::updateSubImage(
    TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
) {
  if (handle.id == 0
      || handle.generation != m_generation
      || data == nullptr
      || x < 0
      || y < 0
      || width <= 0
      || height <= 0
      || x + width > handle.width
      || y + height > handle.height) {
    return false;
  }

  const GLenum glFormat = toGlesFormat(format);
  glBindTexture(GL_TEXTURE_2D, toGlesTexture(handle.id));
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, glFormat, GL_UNSIGNED_BYTE, data);
  return true;
}

void GlesTextureManager::cleanup() {
  if (m_eglDestroyImage != nullptr) {
    for (auto& [tex, image] : m_eglImages) {
      if (image != EGL_NO_IMAGE_KHR) {
        m_eglDestroyImage(m_eglDisplay, image);
      }
    }
  }
  m_eglImages.clear();
  if (!m_textures.empty()) {
    std::vector<GLuint> textures;
    textures.reserve(m_textures.size());
    for (TextureId texture : m_textures) {
      textures.push_back(toGlesTexture(texture));
    }
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    m_textures.clear();
  }
}

void GlesTextureManager::abandonGpuResources() noexcept {
  m_textures.clear();
  m_generation = g_nextTextureGeneration.fetch_add(1);
}

void GlesTextureManager::probeExtensions() {
  const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (ext != nullptr && std::strstr(ext, "GL_EXT_texture_format_BGRA8888") != nullptr) {
    m_hasBgraExt = true;
  }
  ensureDmabufImport();
}

void GlesTextureManager::ensureDmabufImport() {
  if (m_dmabufProbed) {
    return;
  }
  m_dmabufProbed = true;
  if (m_eglDisplay == EGL_NO_DISPLAY) {
    return;
  }
  const char* eglExts = eglQueryString(m_eglDisplay, EGL_EXTENSIONS);
  const char* glExts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  const bool hasDmaImport = eglExts != nullptr && std::strstr(eglExts, "EGL_EXT_image_dma_buf_import") != nullptr;
  const bool hasImageBase = eglExts != nullptr
      && (std::strstr(eglExts, "EGL_KHR_image_base") != nullptr || std::strstr(eglExts, "EGL_KHR_image") != nullptr);
  const bool hasTargetTex = glExts != nullptr && std::strstr(glExts, "GL_OES_EGL_image") != nullptr;
  if (!hasDmaImport || !hasImageBase || !hasTargetTex) {
    kLog.info("dmabuf import unavailable (dma_buf_import={}, image_base={}, oes_egl_image={})", hasDmaImport,
              hasImageBase, hasTargetTex);
    return;
  }
  m_hasDmabufModifiers =
      eglExts != nullptr && std::strstr(eglExts, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;

  m_eglCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  m_eglDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  m_glEglImageTargetTexture2d =
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

  m_hasDmabufImport =
      m_eglCreateImage != nullptr && m_eglDestroyImage != nullptr && m_glEglImageTargetTexture2d != nullptr;
  kLog.info("dmabuf zero-copy import {} (modifiers={})", m_hasDmabufImport ? "enabled" : "unavailable",
            m_hasDmabufModifiers);
}

bool GlesTextureManager::supportsDmabufImport() const {
  return m_hasDmabufImport;
}

TextureHandle GlesTextureManager::importDmabuf(const DmabufImage& image) {
  ensureDmabufImport();
  if (!m_hasDmabufImport || image.width <= 0 || image.height <= 0 || image.planeCount <= 0) {
    return {};
  }

  // Build the EGL_LINUX_DMA_BUF_EXT attribute list (up to 4 planes).
  std::vector<EGLint> attrs;
  attrs.reserve(48);
  attrs.push_back(EGL_WIDTH);
  attrs.push_back(image.width);
  attrs.push_back(EGL_HEIGHT);
  attrs.push_back(image.height);
  attrs.push_back(EGL_LINUX_DRM_FOURCC_EXT);
  attrs.push_back(static_cast<EGLint>(image.fourcc));

  constexpr EGLint planeFd[4] = {
      EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
  constexpr EGLint planeOffset[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                     EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
  constexpr EGLint planePitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                    EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
  constexpr EGLint planeModLo[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
  constexpr EGLint planeModHi[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

  const int planes = std::min(image.planeCount, 4);
  const bool useModifier =
      m_hasDmabufModifiers && image.hasModifier && image.modifier != kDrmModifierInvalid;
  for (int i = 0; i < planes; ++i) {
    attrs.push_back(planeFd[i]);
    attrs.push_back(image.planes[i].fd);
    attrs.push_back(planeOffset[i]);
    attrs.push_back(static_cast<EGLint>(image.planes[i].offset));
    attrs.push_back(planePitch[i]);
    attrs.push_back(static_cast<EGLint>(image.planes[i].stride));
    if (useModifier) {
      attrs.push_back(planeModLo[i]);
      attrs.push_back(static_cast<EGLint>(image.modifier & 0xffffffffU));
      attrs.push_back(planeModHi[i]);
      attrs.push_back(static_cast<EGLint>(image.modifier >> 32));
    }
  }
  attrs.push_back(EGL_NONE);

  const EGLImageKHR eglImage = m_eglCreateImage(
      m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, static_cast<EGLClientBuffer>(nullptr), attrs.data());
  if (eglImage == EGL_NO_IMAGE_KHR) {
    kLog.warn("eglCreateImageKHR(dmabuf) failed (EGL error 0x{:04x})", static_cast<unsigned>(eglGetError()));
    return {};
  }

  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (tex == 0) {
    m_eglDestroyImage(m_eglDisplay, eglImage);
    return {};
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  m_glEglImageTargetTexture2d(GL_TEXTURE_2D, static_cast<GLeglImageOES>(eglImage));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  m_textures.emplace_back(tex);
  m_eglImages.emplace(tex, eglImage);
  return TextureHandle{.id = TextureId{tex}, .width = image.width, .height = image.height};
}

TextureHandle GlesTextureManager::uploadBgra(const std::uint8_t* data, int width, int height, bool mipmap) {
  mipmap = mipmap && globalMipmapsEnabled();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (tex == 0) {
    kLog.warn("glGenTextures failed for {}x{} BGRA texture", width, height);
    return {};
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (!texImage2dChecked(GL_BGRA_EXT, width, height, data, "BGRA")) {
    glDeleteTextures(1, &tex);
    return {};
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (mipmap) {
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  m_textures.emplace_back(tex);
  return TextureHandle{.id = TextureId{tex}, .width = width, .height = height, .generation = m_generation};
}

TextureHandle GlesTextureManager::uploadRgba(const std::uint8_t* data, int width, int height, bool mipmap) {
  return uploadPixels(data, width, height, TextureDataFormat::Rgba, TextureFilter::Linear, mipmap);
}

TextureHandle GlesTextureManager::uploadPixels(
    const std::uint8_t* data, int width, int height, TextureDataFormat format, TextureFilter filter, bool mipmap
) {
  mipmap = mipmap && globalMipmapsEnabled();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (tex == 0) {
    kLog.warn("glGenTextures failed for {}x{} texture", width, height);
    return {};
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  const GLenum glFormat = toGlesFormat(format);
  if (!texImage2dChecked(glFormat, width, height, data, "pixel")) {
    glDeleteTextures(1, &tex);
    return {};
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (mipmap) {
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, toGlesMipmapFilter(filter));
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, toGlesFilter(filter));
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, toGlesFilter(filter));

  m_textures.emplace_back(tex);
  return TextureHandle{.id = TextureId{tex}, .width = width, .height = height, .generation = m_generation};
}
