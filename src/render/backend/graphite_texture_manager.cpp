#include "render/backend/graphite_texture_manager.h"

#include "core/log.h"
#include "render/core/image_decoder.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"
#include "render/graphics_device.h"

#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSize.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("texture");

  std::uint8_t premultiply(std::uint8_t color, std::uint8_t alpha) {
    return static_cast<std::uint8_t>((static_cast<unsigned>(color) * alpha + 127U) / 255U);
  }

  std::vector<std::uint8_t> premultiplyRgba(const std::uint8_t* source, int width, int height) {
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> result(count * 4U);
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
      const std::uint8_t alpha = source[pixel * 4U + 3U];
      result[pixel * 4U] = premultiply(source[pixel * 4U], alpha);
      result[pixel * 4U + 1U] = premultiply(source[pixel * 4U + 1U], alpha);
      result[pixel * 4U + 2U] = premultiply(source[pixel * 4U + 2U], alpha);
      result[pixel * 4U + 3U] = alpha;
    }
    return result;
  }

  std::vector<std::uint8_t> pixelsToRgba(
      const std::uint8_t* source, int width, int height, TextureDataFormat format
  ) {
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> rgba(count * 4U, 255);
    if (source == nullptr) {
      std::ranges::fill(rgba, 0);
      return rgba;
    }
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
      switch (format) {
      case TextureDataFormat::Alpha:
        rgba[pixel * 4U] = 255;
        rgba[pixel * 4U + 1U] = 255;
        rgba[pixel * 4U + 2U] = 255;
        rgba[pixel * 4U + 3U] = source[pixel];
        break;
      case TextureDataFormat::LuminanceAlpha:
        rgba[pixel * 4U] = source[pixel * 2U];
        rgba[pixel * 4U + 1U] = source[pixel * 2U];
        rgba[pixel * 4U + 2U] = source[pixel * 2U];
        rgba[pixel * 4U + 3U] = source[pixel * 2U + 1U];
        break;
      case TextureDataFormat::Rgba:
        std::memcpy(rgba.data() + pixel * 4U, source + pixel * 4U, 4U);
        break;
      }
    }
    return premultiplyRgba(rgba.data(), width, height);
  }
}

struct GraphiteTextureManager::Impl {
  struct Entry {
    std::uint32_t generation = 0;
    skgpu::graphite::BackendTexture backendTexture;
    sk_sp<SkImage> image;
    int width = 0;
    int height = 0;
    TextureFilter filter = TextureFilter::Linear;
    GraphiteExternalImageSynchronization* externalSynchronization = nullptr;
  };

  GraphicsDevice& graphics;
  std::vector<Entry> entries;
  std::vector<std::uint32_t> freeSlots;

  explicit Impl(GraphicsDevice& device) : graphics(device) {}

  Entry* lookup(TextureId id) {
    if (!id.valid() || id.slot() >= entries.size()) {
      return nullptr;
    }
    Entry& entry = entries[id.slot()];
    return entry.image != nullptr && id.generation() == entry.generation ? &entry : nullptr;
  }

  const Entry* lookup(TextureId id) const {
    return const_cast<Impl*>(this)->lookup(id);
  }

  void deleteTexture(Entry& entry) {
    entry.image.reset();
    if (entry.backendTexture.isValid() && graphics.graphiteContext() != nullptr) {
      graphics.graphiteContext()->deleteBackendTexture(entry.backendTexture);
    }
    entry.backendTexture = {};
    entry.width = 0;
    entry.height = 0;
    entry.externalSynchronization = nullptr;
  }

  TextureHandle upload(
      const std::uint8_t* premultiplied, int width, int height, TextureFilter filter, bool mipmap
  ) {
    if (premultiplied == nullptr || width <= 0 || height <= 0 || graphics.recorder() == nullptr) {
      return {};
    }
    if (freeSlots.empty() && entries.size() >= std::numeric_limits<std::uint32_t>::max()) {
      kLog.error("texture slot space exhausted");
      return {};
    }
    const auto generation = graphics.allocateTextureGeneration();
    if (!generation) {
      kLog.error("texture generation space exhausted; refusing to alias a stale handle");
      return {};
    }
    const bool useMipmaps = mipmap && TextureManager::globalMipmapsEnabled();
    const SkImageInfo imageInfo = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB()
    );
    const SkPixmap pixmap(imageInfo, premultiplied, imageInfo.minRowBytes());

    skgpu::graphite::VulkanTextureInfo textureInfo;
    textureInfo.fSampleCount = skgpu::graphite::SampleCount::k1;
    textureInfo.fMipmapped = useMipmaps ? skgpu::Mipmapped::kYes : skgpu::Mipmapped::kNo;
    textureInfo.fFormat = VK_FORMAT_R8G8B8A8_UNORM;
    textureInfo.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    textureInfo.fImageUsageFlags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    textureInfo.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto backend = graphics.recorder()->createBackendTexture(
        SkISize::Make(width, height), skgpu::graphite::TextureInfos::MakeVulkan(textureInfo)
    );
    if (!backend.isValid()) {
      return {};
    }
    // Static mipmapped images are uploaded through TextureFromImage so Skia
    // generates the complete mip chain. Dynamic textures stay single-level and
    // use updateBackendTexture in place.
    sk_sp<SkImage> image;
    if (useMipmaps) {
      const auto raster = SkImages::RasterFromPixmapCopy(pixmap);
      image = SkImages::TextureFromImage(
          graphics.recorder(), raster, SkImage::RequiredProperties{.fMipmapped = true}
      );
      graphics.recorder()->deleteBackendTexture(backend);
      backend = {};
    } else {
      if (!graphics.recorder()->updateBackendTexture(backend, &pixmap, 1)) {
        graphics.recorder()->deleteBackendTexture(backend);
        return {};
      }
      image = SkImages::WrapTexture(graphics.recorder(), backend, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    }
    if (image == nullptr) {
      if (backend.isValid()) {
        graphics.recorder()->deleteBackendTexture(backend);
      }
      return {};
    }

    std::uint32_t slot = 0;
    if (freeSlots.empty()) {
      slot = static_cast<std::uint32_t>(entries.size());
      entries.emplace_back();
    } else {
      slot = freeSlots.back();
      freeSlots.pop_back();
    }
    Entry& entry = entries[slot];
    entry.generation = *generation;
    entry.backendTexture = backend;
    entry.image = std::move(image);
    entry.width = width;
    entry.height = height;
    entry.filter = filter;
    return TextureHandle{
        .id = TextureId::fromSlot(slot, entry.generation),
        .width = width,
        .height = height,
    };
  }

  TextureHandle adopt(
      sk_sp<SkImage> externalImage, int width, int height, TextureFilter filter,
      GraphiteExternalImageSynchronization* synchronization
  ) {
    if (externalImage == nullptr || width <= 0 || height <= 0) {
      return {};
    }
    if (freeSlots.empty() && entries.size() >= std::numeric_limits<std::uint32_t>::max()) {
      kLog.error("texture slot space exhausted");
      return {};
    }
    const auto generation = graphics.allocateTextureGeneration();
    if (!generation) {
      kLog.error("texture generation space exhausted; refusing to alias a stale handle");
      return {};
    }
    std::uint32_t slot = 0;
    if (freeSlots.empty()) {
      slot = static_cast<std::uint32_t>(entries.size());
      entries.emplace_back();
    } else {
      slot = freeSlots.back();
      freeSlots.pop_back();
    }
    Entry& entry = entries[slot];
    entry.generation = *generation;
    entry.image = std::move(externalImage);
    entry.width = width;
    entry.height = height;
    entry.filter = filter;
    entry.externalSynchronization = synchronization;
    return TextureHandle{
        .id = TextureId::fromSlot(slot, entry.generation),
        .width = width,
        .height = height,
    };
  }
};

GraphiteTextureManager::GraphiteTextureManager(GraphicsDevice& graphics) : m_impl(std::make_unique<Impl>(graphics)) {}

GraphiteTextureManager::~GraphiteTextureManager() { cleanup(); }

TextureHandle GraphiteTextureManager::loadFromFile(const std::string& path, int targetSize, bool mipmap) {
  auto loaded = loadImageFile(path, targetSize);
  if (!loaded) {
    kLog.warn("failed to load image: {} ({})", ImageSourceLog::describe(path), loaded.error());
    return {};
  }
  return loadFromRgba(loaded->rgba.data(), loaded->width, loaded->height, mipmap);
}

TextureHandle
GraphiteTextureManager::loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap) {
  auto decoded = decodeRasterImage(data, size);
  if (!decoded) {
    return {};
  }
  return loadFromRgba(decoded->pixels.data(), decoded->width, decoded->height, mipmap);
}

TextureHandle GraphiteTextureManager::loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return {};
  }
  const auto premultiplied = premultiplyRgba(data, width, height);
  return m_impl->upload(premultiplied.data(), width, height, TextureFilter::Linear, mipmap);
}

TextureHandle GraphiteTextureManager::loadFromRaw(
    const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format, bool mipmap
) {
  if (data == nullptr || width <= 0 || height <= 0 || stride <= 0
      || size < static_cast<std::size_t>(stride) * static_cast<std::size_t>(height)) {
    return {};
  }
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  const int channels = format == PixmapFormat::RGB || format == PixmapFormat::BGR ? 3 : 4;
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* row = data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
    for (int x = 0; x < width; ++x) {
      const std::uint8_t* pixel = row + static_cast<std::size_t>(x * channels);
      std::uint8_t* out = rgba.data() + (static_cast<std::size_t>(y * width + x) * 4U);
      switch (format) {
      case PixmapFormat::RGBA:
        std::memcpy(out, pixel, 4U);
        break;
      case PixmapFormat::BGRA:
        out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0]; out[3] = pixel[3];
        break;
      case PixmapFormat::ARGB:
        out[0] = pixel[1]; out[1] = pixel[2]; out[2] = pixel[3]; out[3] = pixel[0];
        break;
      case PixmapFormat::RGB:
        out[0] = pixel[0]; out[1] = pixel[1]; out[2] = pixel[2]; out[3] = 255;
        break;
      case PixmapFormat::BGR:
        out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0]; out[3] = 255;
        break;
      }
    }
  }
  return loadFromRgba(rgba.data(), width, height, mipmap);
}

TextureHandle GraphiteTextureManager::loadFromPixels(
    const std::uint8_t* data, int width, int height, TextureDataFormat format, TextureFilter filter, bool mipmap
) {
  if (width <= 0 || height <= 0) {
    return {};
  }
  const auto rgba = pixelsToRgba(data, width, height, format);
  return m_impl->upload(rgba.data(), width, height, filter, mipmap);
}

TextureHandle GraphiteTextureManager::createEmpty(
    int width, int height, TextureDataFormat format, TextureFilter filter
) {
  return loadFromPixels(nullptr, width, height, format, filter, false);
}

TextureHandle GraphiteTextureManager::createBgraSurface(int width, int height) {
  return createEmpty(width, height, TextureDataFormat::Rgba, TextureFilter::Linear);
}

bool GraphiteTextureManager::updateBgraSurface(
    TextureHandle& handle, const std::uint8_t* data, int width, int height
) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++pixel) {
    rgba[pixel * 4U] = data[pixel * 4U + 2U];
    rgba[pixel * 4U + 1U] = data[pixel * 4U + 1U];
    rgba[pixel * 4U + 2U] = data[pixel * 4U];
    rgba[pixel * 4U + 3U] = data[pixel * 4U + 3U];
  }
  return replace(handle, rgba.data(), width, height, TextureDataFormat::Rgba, TextureFilter::Linear, false);
}

bool GraphiteTextureManager::replace(
    TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
    TextureFilter filter, bool mipmap
) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  auto* entry = m_impl->lookup(handle.id);
  const auto rgba = pixelsToRgba(data, width, height, format);
  if (entry != nullptr && entry->backendTexture.isValid() && entry->width == width && entry->height == height
      && !mipmap) {
    const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const SkPixmap pixmap(info, rgba.data(), info.minRowBytes());
    if (m_impl->graphics.recorder()->updateBackendTexture(entry->backendTexture, &pixmap, 1)) {
      entry->filter = filter;
      handle.width = width;
      handle.height = height;
      return true;
    }
  }
  TextureHandle replacement = m_impl->upload(rgba.data(), width, height, filter, mipmap);
  if (!replacement.valid()) {
    return false;
  }
  unload(handle);
  handle = replacement;
  return true;
}

bool GraphiteTextureManager::updateSubImage(
    TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
) {
  auto* entry = m_impl->lookup(handle.id);
  if (entry == nullptr || !entry->backendTexture.isValid() || data == nullptr || x != 0 || y != 0
      || width != entry->width || height != entry->height) {
    // Graphite's public upload API updates full backend textures. Current
    // dynamic users already replace their complete graph/audio row; reject a
    // partial update rather than introducing an untracked staging image.
    return false;
  }
  const auto rgba = pixelsToRgba(data, width, height, format);
  const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  const SkPixmap pixmap(info, rgba.data(), info.minRowBytes());
  return m_impl->graphics.recorder()->updateBackendTexture(entry->backendTexture, &pixmap, 1);
}

void GraphiteTextureManager::unload(TextureHandle& handle) {
  auto* entry = m_impl->lookup(handle.id);
  if (entry != nullptr) {
    const std::uint32_t slot = handle.id.slot();
    m_impl->deleteTexture(*entry);
    entry->generation = 0;
    m_impl->freeSlots.push_back(slot);
  }
  handle = {};
}

void GraphiteTextureManager::cleanup() {
  if (m_impl == nullptr) {
    return;
  }
  for (auto& entry : m_impl->entries) {
    m_impl->deleteTexture(entry);
    entry.generation = 0;
  }
  m_impl->freeSlots.clear();
  m_impl->freeSlots.reserve(m_impl->entries.size());
  for (std::uint32_t slot = 0; slot < m_impl->entries.size(); ++slot) {
    m_impl->freeSlots.push_back(slot);
  }
}

SkImage* GraphiteTextureManager::image(TextureId id) const noexcept {
  const auto* entry = m_impl != nullptr ? m_impl->lookup(id) : nullptr;
  return entry != nullptr ? entry->image.get() : nullptr;
}

TextureFilter GraphiteTextureManager::filter(TextureId id) const noexcept {
  const auto* entry = m_impl != nullptr ? m_impl->lookup(id) : nullptr;
  return entry != nullptr ? entry->filter : TextureFilter::Linear;
}

TextureHandle GraphiteTextureManager::adoptExternalImage(
    sk_sp<SkImage> image, int width, int height, TextureFilter filter,
    GraphiteExternalImageSynchronization* synchronization
) {
  return m_impl != nullptr
      ? m_impl->adopt(std::move(image), width, height, filter, synchronization)
      : TextureHandle{};
}

GraphiteExternalImageSynchronization*
GraphiteTextureManager::externalSynchronization(TextureId id) const noexcept {
  const auto* entry = m_impl != nullptr ? m_impl->lookup(id) : nullptr;
  return entry != nullptr ? entry->externalSynchronization : nullptr;
}

bool GraphiteTextureManager::rebindExternalImage(
    TextureHandle& handle, sk_sp<SkImage> image, int width, int height
) {
  if (m_impl == nullptr || image == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  auto* entry = m_impl->lookup(handle.id);
  if (entry == nullptr || entry->backendTexture.isValid()) {
    return false;
  }
  entry->image = std::move(image);
  entry->width = width;
  entry->height = height;
  handle.width = width;
  handle.height = height;
  return true;
}

void GraphiteTextureManager::invalidateAll() {
  cleanup();
}
