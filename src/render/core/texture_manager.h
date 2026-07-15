#pragma once

#include "render/core/texture_handle.h"

#include <cstddef>
#include <cstdint>
#include <string>

enum class PixmapFormat {
  RGBA, // Red, Green, Blue, Alpha
  BGRA, // Blue, Green, Red, Alpha
  ARGB, // Alpha, Red, Green, Blue
  RGB,  // Red, Green, Blue (No Alpha)
  BGR   // Blue, Green, Red (No Alpha)
};

enum class TextureDataFormat {
  Alpha,
  LuminanceAlpha,
  Rgba,
};

enum class TextureFilter {
  Nearest,
  Linear,
};

class TextureManager {
public:
  virtual ~TextureManager() = default;

  TextureManager(const TextureManager&) = delete;
  TextureManager& operator=(const TextureManager&) = delete;

  static void setGlobalMipmapsEnabled(bool enabled) { s_mipmapsEnabled = enabled; }
  [[nodiscard]] static bool globalMipmapsEnabled() { return s_mipmapsEnabled; }

  [[nodiscard]] virtual TextureHandle
  loadFromFile(const std::string& path, int targetSize = 0, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle
  loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle
  loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle loadFromRaw(
      const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format,
      bool mipmap = false
  ) = 0;
  [[nodiscard]] virtual TextureHandle loadFromPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) = 0;
  [[nodiscard]] virtual TextureHandle
  createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter = TextureFilter::Linear) = 0;

  // Streaming BGRA surface for high-rate full-frame sources (embedded browser).
  // createBgraSurface allocates the texture once; updateBgraSurface re-uploads a
  // full BGRA frame in place — no reallocation, and no CPU swizzle where
  // GL_EXT_texture_format_BGRA8888 is available. The frame must match the
  // surface dimensions and be tightly packed (stride == width*4).
  [[nodiscard]] virtual TextureHandle createBgraSurface(int width, int height) = 0;
  virtual bool updateBgraSurface(TextureHandle& handle, const std::uint8_t* data, int width, int height) = 0;

  // Zero-copy import of an externally-produced dmabuf (e.g. an embedded
  // browser's GPU-rendered shared texture) as a GL texture backed by an
  // EGLImage — no pixel copy, no upload. The caller retains ownership of the
  // fds and may close them once import returns (the EGLImage takes its own
  // reference). unload() releases the EGLImage. Returns an invalid handle if
  // dmabuf import is unavailable (see supportsDmabufImport()).
  struct DmabufPlane {
    int fd = -1;
    std::uint32_t stride = 0;
    std::uint64_t offset = 0;
  };
  struct DmabufImage {
    int width = 0;
    int height = 0;
    std::uint32_t fourcc = 0;   // DRM FourCC (e.g. DRM_FORMAT_ARGB8888)
    std::uint64_t modifier = 0; // DRM format modifier
    bool hasModifier = false;
    int planeCount = 0;
    DmabufPlane planes[4];
  };
  [[nodiscard]] virtual bool supportsDmabufImport() const = 0;
  [[nodiscard]] virtual TextureHandle importDmabuf(const DmabufImage& image) = 0;
  virtual bool replace(
      TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) = 0;
  virtual bool updateSubImage(
      TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
  ) = 0;
  virtual void unload(TextureHandle& handle) = 0;
  virtual void cleanup() = 0;
  // Forget GL names without deleting them. Used after a robust-context reset,
  // when every object in the old share group is already invalid.
  virtual void abandonGpuResources() noexcept = 0;

  virtual void probeExtensions() = 0;

protected:
  TextureManager() = default;

private:
  static inline bool s_mipmapsEnabled = true;
};
