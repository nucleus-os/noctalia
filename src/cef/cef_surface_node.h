#pragma once

#include "render/scene/node.h"

#include <functional>

class CefService;
class ImageNode;
class InputArea;
class Renderer;
class TextureManager;

// Scene node that displays the embedded CEF browser and forwards input to it.
// Deliberately CEF-free: it talks only to CefService's opaque API. Holds no
// browser state itself — the browser + texture live in the app-level CefService
// and survive this node being destroyed and rebuilt on panel close/reopen.
class CefSurfaceNode : public Node {
public:
  explicit CefSurfaceNode(CefService& service);
  ~CefSurfaceNode() override;

  // Wire the display live: requestRedraw is invoked on the main thread whenever
  // a fresh browser frame is ready, so the owner can schedule a repaint. Marks
  // the browser attached (it resumes painting + takes focus).
  void attach(std::function<void()> requestRedraw, std::function<void()> refreshCursor);
  // Detach the display: the browser keeps running (audio continues) but stops
  // painting until re-attached.
  void detach();

  // Adopt the latest browser frame into the image texture. The caller must be
  // inside the Graphite frame that will sample it. Returns true when a fresh
  // frame was adopted.
  bool syncTexture(TextureManager& textures);
  [[nodiscard]] InputArea* inputArea() const noexcept { return m_input; }

protected:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;

private:
  void wireInput();

  CefService& m_service;
  ImageNode* m_image = nullptr;
  InputArea* m_input = nullptr;
  bool m_attached = false;
};
