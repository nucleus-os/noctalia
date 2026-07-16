#pragma once

#include "ui/controls/flex.h"
#include "ui/signal.h"

#include <string>
#include <vector>

class Label;

class MarkdownView : public Flex {
public:
  MarkdownView();
  void setMarkdown(const std::string& markdown, float scale);
  void clear();
  void trackWrappableLabel(Label* label) { m_wrappableLabels.push_back(label); }

protected:
  void doLayout(Renderer& renderer) override;

private:
  float m_scale = 1.0f;
  std::string m_markdown;
  std::vector<Label*> m_wrappableLabels;
  Signal<>::ScopedConnection m_paletteConnection;
};
