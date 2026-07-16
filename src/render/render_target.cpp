#include "render/render_target.h"

#include "render/backend/render_backend.h"
#include "render/render_context.h"

#include <stdexcept>
#include <utility>

RenderTarget::RenderTarget() = default;

RenderTarget::~RenderTarget() { destroy(); }

void RenderTarget::create(wl_surface* surface, RenderContext& context) {
  destroy();
  m_surface = surface;
  m_context = &context;
  context.registerTarget(*this);
  try {
    m_surfaceTarget = context.backend().createSurfaceTarget(surface);
    if (m_surfaceTarget == nullptr) {
      throw std::runtime_error("render backend failed to create a surface target");
    }
    m_surfaceTarget->setPresentationCallback(m_presentationCallback);
  } catch (...) {
    context.unregisterTarget(*this);
    m_context = nullptr;
    m_surface = nullptr;
    throw;
  }
}

void RenderTarget::create(wl_surface* surface, RenderBackend& backend) {
  destroy();
  m_surface = surface;
  m_surfaceTarget = backend.createSurfaceTarget(surface);
  if (m_surfaceTarget == nullptr) {
    throw std::runtime_error("render backend failed to create a surface target");
  }
}

void RenderTarget::resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) {
  if (bufferWidth == 0 || bufferHeight == 0) {
    return;
  }

  m_bufferWidth = bufferWidth;
  m_bufferHeight = bufferHeight;

  if (m_surfaceTarget != nullptr) {
    m_surfaceTarget->resize(bufferWidth, bufferHeight);
  }
}

void RenderTarget::setPresentationCallback(SurfacePresentationCallback callback) {
  m_presentationCallback = callback;
  if (m_surfaceTarget != nullptr) {
    m_surfaceTarget->setPresentationCallback(std::move(callback));
  }
}

bool RenderTarget::isReady() const noexcept { return m_surfaceTarget != nullptr && m_surfaceTarget->isReady(); }

void RenderTarget::destroy() {
  if (m_surfaceTarget != nullptr) {
    m_surfaceTarget->destroy();
    m_surfaceTarget.reset();
  }
  if (m_context != nullptr) {
    m_context->unregisterTarget(*this);
    m_context = nullptr;
  }
  m_surface = nullptr;
  m_presentationCallback = {};
  m_bufferWidth = 0;
  m_bufferHeight = 0;
  m_logicalWidth = 0;
  m_logicalHeight = 0;
}

void RenderTarget::suspendForGraphicsDeviceRebuild() {
  if (m_surfaceTarget != nullptr) {
    m_surfaceTarget->destroy();
    m_surfaceTarget.reset();
  }
}

void RenderTarget::resumeAfterGraphicsDeviceRebuild(RenderContext& context) {
  if (m_context != &context || m_surface == nullptr || m_surfaceTarget != nullptr) {
    return;
  }
  m_surfaceTarget = context.backend().createSurfaceTarget(m_surface);
  if (m_surfaceTarget == nullptr) {
    throw std::runtime_error("render backend failed to recreate a surface target after device loss");
  }
  m_surfaceTarget->setPresentationCallback(m_presentationCallback);
  if (m_bufferWidth > 0 && m_bufferHeight > 0) {
    m_surfaceTarget->resize(m_bufferWidth, m_bufferHeight);
  }
}
