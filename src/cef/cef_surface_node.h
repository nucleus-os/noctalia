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
  void attach(std::function<void()> requestRedraw);
  // Detach the display: the browser keeps running (audio continues) but stops
  // painting until re-attached.
  void detach();

  // Upload the latest browser frame into the image texture. CONTRACT: the
  // caller must have made the GL context current first (as every other
  // out-of-renderScene upload in the shell does). Returns true if a new frame
  // was uploaded.
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
};
