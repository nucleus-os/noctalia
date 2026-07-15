#pragma once

#include "render/core/texture_manager.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class GlesTextureManager final : public TextureManager {
public:
  GlesTextureManager();
  ~GlesTextureManager() override;

  GlesTextureManager(const GlesTextureManager&) = delete;
  GlesTextureManager& operator=(const GlesTextureManager&) = delete;

  [[nodiscard]] TextureHandle loadFromFile(const std::string& path, int targetSize = 0, bool mipmap = false) override;
  [[nodiscard]] TextureHandle
  loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap = false) override;
  [[nodiscard]] TextureHandle
  loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap = false) override;
  [[nodiscard]] TextureHandle loadFromRaw(
      const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format,
      bool mipmap = false
  ) override;
  [[nodiscard]] TextureHandle loadFromPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) override;
  [[nodiscard]] TextureHandle
  createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter = TextureFilter::Linear) override;
  [[nodiscard]] TextureHandle createBgraSurface(int width, int height) override;
  bool updateBgraSurface(TextureHandle& handle, const std::uint8_t* data, int width, int height) override;
  bool replace(
      TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) override;
  bool updateSubImage(
      TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
  ) override;

  [[nodiscard]] bool supportsDmabufImport() const override;
  [[nodiscard]] TextureHandle importDmabuf(const DmabufImage& image) override;

  void unload(TextureHandle& handle) override;
  void cleanup() override;
  void abandonGpuResources() noexcept override;

  void probeExtensions() override;

  // EGLDisplay for dmabuf/EGLImage import (set by the backend in initialize()).
  void setEglDisplay(EGLDisplay display) noexcept { m_eglDisplay = display; }

private:
  TextureHandle decodeEncodedRaster(
      const std::uint8_t* data, std::size_t size, const std::string* debugPath = nullptr, bool mipmap = false
  );
  TextureHandle uploadPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  );
  TextureHandle uploadRgba(const std::uint8_t* data, int width, int height, bool mipmap = false);
  TextureHandle uploadBgra(const std::uint8_t* data, int width, int height, bool mipmap = false);

  // Lazily resolves the EGL/GL entry points for dmabuf import.
  void ensureDmabufImport();

  std::vector<TextureId> m_textures;
  std::uint64_t m_generation = 0;
  bool m_hasBgraExt = false;
  // Reused RGBA scratch for the rare no-BGRA-extension streaming fallback.
  std::vector<std::uint8_t> m_bgraFallbackScratch;

  // dmabuf/EGLImage zero-copy import state.
  EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
  bool m_dmabufProbed = false;
  bool m_hasDmabufImport = false;
  bool m_hasDmabufModifiers = false;
  PFNEGLCREATEIMAGEKHRPROC m_eglCreateImage = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImage = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEglImageTargetTexture2d = nullptr;
  // GL texture id -> its backing EGLImage, so unload() can release the image.
  std::unordered_map<std::uint32_t, EGLImageKHR> m_eglImages;
};
