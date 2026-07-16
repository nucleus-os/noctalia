#include "render/core/shared_texture_cache.h"

#include "core/log.h"
#include "render/core/texture_manager.h"

namespace {
  constexpr Logger kLog("texcache");
} // namespace

SharedTextureCache::~SharedTextureCache() {
  if (m_textureManager != nullptr) {
    for (auto& [path, entry] : m_entries) {
      (void)path;
      m_textureManager->unload(entry.handle);
    }
  }
}

void SharedTextureCache::initialize(TextureManager& textures) { m_textureManager = &textures; }

void SharedTextureCache::abandonGpuResources() noexcept {
  for (auto& [path, entry] : m_entries) {
    (void)path;
    entry.handle = {};
  }
  m_textureManager = nullptr;
}

TextureHandle SharedTextureCache::acquire(const std::string& path) {
  if (path.empty() || m_textureManager == nullptr) {
    return {};
  }

  auto it = m_entries.find(path);
  if (it != m_entries.end()) {
    ++it->second.refCount;
    kLog.info("hit {} (refCount={})", path, it->second.refCount);
    if (it->second.handle.id == 0) {
      it->second.handle = m_textureManager->loadFromFile(path, 0, true);
      if (it->second.handle.id == 0) {
        return {};
      }
    }
    return it->second.handle;
  }

  auto handle = m_textureManager->loadFromFile(path, 0, true);
  if (handle.id == 0) {
    return handle;
  }

  m_entries[path] = Entry{.handle = handle, .refCount = 1};
  kLog.info("uploaded {}", path);
  return handle;
}

TextureHandle SharedTextureCache::peek(const std::string& path) const {
  const auto it = m_entries.find(path);
  return it != m_entries.end() ? it->second.handle : TextureHandle{};
}

void SharedTextureCache::release(TextureHandle& handle, const std::string& path) {
  if (handle.id == 0 || path.empty() || m_textureManager == nullptr) {
    handle = {};
    return;
  }

  auto it = m_entries.find(path);
  if (it == m_entries.end()) {
    handle = {};
    return;
  }

  --it->second.refCount;
  if (it->second.refCount <= 0) {
    // If the context can't be bound (lost on resume) the GPU object is already gone; skip the
    // unload but still drop the tracking entry so it gets re-uploaded fresh when next acquired.
    m_textureManager->unload(it->second.handle);
    m_entries.erase(it);
    kLog.info("evicted {}", path);
  }

  handle = {};
}

void SharedTextureCache::reloadResidentTextures() {
  if (m_textureManager == nullptr || m_entries.empty()) {
    return;
  }

  for (auto& [path, entry] : m_entries) {
    if (entry.handle.id != 0) {
      m_textureManager->unload(entry.handle);
    }
    entry.handle = m_textureManager->loadFromFile(path, 0, true);
    if (entry.handle.id != 0) {
      kLog.info("reuploaded {}", path);
    } else {
      kLog.warn("failed to reupload {}", path);
    }
  }
}
