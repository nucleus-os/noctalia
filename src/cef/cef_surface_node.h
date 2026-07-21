#pragma once

#include "render/scene/node.h"

#include <functional>

class CefBrowserSession;
class ImageNode;
class InputArea;
class Renderer;
class TextureManager;

// Scene node that displays the embedded CEF browser and forwards input to it.
// Deliberately CEF-free: it talks only to one CefBrowserSession. Holds no
// browser state itself, so that session and its texture survive this node being
// destroyed and rebuilt on panel close/reopen.
class CefSurfaceNode : public Node {
public:
  explicit CefSurfaceNode(CefBrowserSession& session);
  ~CefSurfaceNode() override;

  // Wire the display live: requestRedraw is invoked on the main thread whenever
  // a fresh browser frame is ready, and requestFrameOpportunity arms one
  // callback-only Wayland tick when CEF needs another compositor-paced begin
  // frame. Marks the browser attached (it resumes painting + takes focus).
  void attach(
      std::function<void()> requestRedraw, std::function<void()> requestFrameOpportunity,
      std::function<void()> refreshCursor
  );
  // Detach this scene consumer: hides it and enters the parked-frame policy.
  void detach();

  // Adopt the latest browser frame into the image texture. The caller must be
  // inside the Graphite frame that will sample it. Returns true when a fresh
  // frame was adopted.
  bool syncTexture(TextureManager& textures);
  [[nodiscard]] InputArea* inputArea() const noexcept { return m_input; }
  void setCornerRadius(float radius);

protected:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;

private:
  void wireInput();

  CefBrowserSession& m_session;
  ImageNode* m_image = nullptr;
  InputArea* m_input = nullptr;
  bool m_attached = false;
};
