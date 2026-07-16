#pragma once

#include "render/core/texture_handle.h"

#include <memory>
#include <string>
#include <unordered_map>

class TextureManager;

// Path-keyed, refcounted cache backed by the process Graphite texture manager.
class SharedTextureCache {
public:
  SharedTextureCache() = default;
  ~SharedTextureCache();

  SharedTextureCache(const SharedTextureCache&) = delete;
  SharedTextureCache& operator=(const SharedTextureCache&) = delete;

  void initialize(TextureManager& textures);

  [[nodiscard]] bool shared() const noexcept { return m_textureManager != nullptr; }

  [[nodiscard]] TextureHandle acquire(const std::string& path);
  [[nodiscard]] TextureHandle peek(const std::string& path) const;
  void release(TextureHandle& handle, const std::string& path);
  void abandonGpuResources() noexcept;
  void reloadResidentTextures();

private:
  struct Entry {
    TextureHandle handle;
    int refCount = 0;
  };

  TextureManager* m_textureManager = nullptr;
  std::unordered_map<std::string, Entry> m_entries;
};
