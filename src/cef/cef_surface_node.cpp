#include "cef/cef_surface_node.h"

#include "cef/cef_service.h"
#include "core/tracy_latency.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"

#include <linux/input-event-codes.h>
#include <memory>

namespace {
// wl axis value -> CEF wheel delta. wl positive = scroll down; Chromium wheel
// deltaY positive = scroll up, so negate. ~120 units per detent.
constexpr float kWheelStep = 120.0f;

int cefButtonFromLinux(std::uint32_t button) {
  switch (button) {
    case BTN_MIDDLE: return 1;
    case BTN_RIGHT: return 2;
    case BTN_LEFT:
    default: return 0;
  }
}
} // namespace

CefSurfaceNode::CefSurfaceNode(CefService& service) : m_service(service) {
  auto image = std::make_unique<ImageNode>();
  m_image = image.get();
  addChild(std::move(image));

  auto input = std::make_unique<InputArea>();
  m_input = input.get();
  addChild(std::move(input));

  wireInput();
}

CefSurfaceNode::~CefSurfaceNode() {
  detach();
}

void CefSurfaceNode::setCornerRadius(float radius) {
  if (m_image != nullptr) {
    m_image->setRadius(radius);
  }
}

void CefSurfaceNode::wireInput() {
  m_input->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_MIDDLE, BTN_RIGHT}));
  m_input->setFocusable(true);
  // The browser is itself a keyboard-focus container. InputDispatcher normally
  // clears focus from button-like areas on pointer release; doing that here
  // sends CEF SetFocus(false) immediately after the DOM field handles the
  // click, which makes text fields visibly focus and then blur.
  m_input->setRetainsFocusOnPointerRelease(true);
  m_input->setCursorShape(0);

  m_input->setOnMotion([this](const InputArea::PointerData& p) {
    tracy_latency::inputReceived(tracy_latency::InputKind::PointerMove);
    m_service.sendMouseMove(p.localX, p.localY, 0, false);
  });
  m_input->setOnEnter([this](const InputArea::PointerData& p) {
    tracy_latency::inputReceived(tracy_latency::InputKind::PointerMove);
    m_service.sendMouseMove(p.localX, p.localY, 0, false);
    m_service.flushMouseMove();
  });
  m_input->setOnLeave([this]() {
    tracy_latency::inputReceived(tracy_latency::InputKind::PointerMove);
    m_service.sendMouseMove(0.0f, 0.0f, 0, true);
  });
  m_input->setOnPress([this](const InputArea::PointerData& p) {
    if (p.pressed) {
      tracy_latency::inputReceived(tracy_latency::InputKind::PointerButton);
    }
    m_service.sendMouseButton(p.localX, p.localY, cefButtonFromLinux(p.button), p.pressed, 1, 0);
  });
  m_input->setOnAxisHandler([this](const InputArea::PointerData& p) {
    tracy_latency::inputReceived(tracy_latency::InputKind::PointerWheel);
    m_service.sendMouseWheel(p.localX, p.localY, 0.0f, -p.scrollDelta(kWheelStep), 0);
    return true;
  });

  m_input->setOnKeyDown([this](const InputArea::KeyData& k) {
    tracy_latency::inputReceived(tracy_latency::InputKind::Key);
    m_service.sendKey(k.sym, k.utf32, k.modifiers, true);
  });
  m_input->setOnKeyUp([this](const InputArea::KeyData& k) {
    m_service.sendKey(k.sym, k.utf32, k.modifiers, false);
  });
  m_input->setOnFocusGain([this]() { m_service.setFocus(true); });
  m_input->setOnFocusLoss([this]() { m_service.setFocus(false); });
}

void CefSurfaceNode::attach(
    std::function<void()> requestRedraw, std::function<void()> requestFrameOpportunity,
    std::function<void()> refreshCursor
) {
  m_service.setFrameReadyCallback(std::move(requestRedraw));
  m_service.setFrameOpportunityCallback(std::move(requestFrameOpportunity));
  m_service.setCursorCallback([this, refreshCursor = std::move(refreshCursor)](std::uint32_t shape) {
    if (m_input != nullptr) {
      m_input->setCursorShape(shape);
      // CEF chooses cursors asynchronously. Re-apply the new shape now so a
      // pointer-motion dispatch cannot keep restoring the previous shape.
      if (refreshCursor) {
        refreshCursor();
      }
    }
  });
  if (!m_attached) {
    m_service.setDisplayAttached(true);
    m_attached = true;
  }
}

void CefSurfaceNode::detach() {
  if (!m_attached) {
    return;
  }
  m_attached = false;
  m_service.setDisplayAttached(false);
  m_service.setFrameReadyCallback(nullptr);
  m_service.setFrameOpportunityCallback(nullptr);
  m_service.setCursorCallback(nullptr);
}

void CefSurfaceNode::doLayout(Renderer& renderer) {
  const auto w = static_cast<int>(width());
  const auto h = static_cast<int>(height());

  m_service.setDeviceScale(renderer.renderScale());
  m_service.ensureBrowser(w, h);
  m_service.resize(w, h);

  if (m_image != nullptr) {
    m_image->setPosition(0.0f, 0.0f);
    m_image->setSize(width(), height());
  }
  if (m_input != nullptr) {
    m_input->setPosition(0.0f, 0.0f);
    m_input->setSize(width(), height());
  }
}

bool CefSurfaceNode::syncTexture(TextureManager& textures) {
  const bool uploaded = m_service.uploadIfDirty(textures);
  const TextureHandle handle = m_service.currentTexture();
  if (m_image != nullptr) {
    m_image->setTextureId(handle.valid() ? handle.id : TextureId{});
    if (handle.valid()) {
      m_image->setTextureSize(handle.width, handle.height);
    }
  }
  return uploaded;
}

void CefSurfaceNode::doInvalidateGpuResources(Renderer& /*renderer*/) {
  m_service.invalidateGpuTexture();
  if (m_image != nullptr) {
    m_image->setTextureId({});
  }
}
