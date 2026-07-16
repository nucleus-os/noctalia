#include "render/text/grapheme_breaks.h"
#include "render/text/skparagraph_text_renderer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main() {
  SkParagraphTextRenderer renderer;
  const std::string text = "abc \xD7\x90\xD7\x91\xD7\x92";
  const auto offsets = text::graphemeBreaks(text);
  std::vector<TextCursorStop> stops;
  renderer.measureCursorStopsWrapped(text, 20.0f, offsets, 200.0f, stops, FontWeight::Normal);
  if (stops.size() != offsets.size()) {
    std::cerr << "paragraph adapter did not return one stop per grapheme boundary\n";
    return 1;
  }
  const auto boundary = std::ranges::find(offsets, 4U);
  if (boundary == offsets.end()) {
    std::cerr << "mixed-bidi test boundary was not a grapheme stop\n";
    return 1;
  }
  const auto index = static_cast<std::size_t>(boundary - offsets.begin());
  if (!stops[index].alternateValid
      || std::abs(stops[index].x - stops[index].alternateX) < 1.0f
      || stops[index].height <= 0.0f || stops[index].alternateHeight <= 0.0f) {
    std::cerr << "paragraph adapter discarded a distinct bidi caret affinity\n";
    return 1;
  }
  if (!stops[index].rangeValid || stops[index].trailingX == stops[index].x) {
    std::cerr << "paragraph adapter discarded shaped selection geometry\n";
    return 1;
  }

  const std::vector<StyledTextRun> styled{
      {.text = "bold", .bold = true},
      {.text = " code", .monospace = true, .underline = true},
      {.text = " deleted", .italic = true, .strikeThrough = true},
  };
  const auto styledMetrics = renderer.measureStyled(
      styled, 18.0f, FontWeight::Normal, 400.0f, 1, TextAlign::Start, {}, TextEllipsize::None);
  if (styledMetrics.width <= 0.0f || styledMetrics.lineCount != 1) {
    std::cerr << "structured styled runs did not produce a one-line paragraph\n";
    return 1;
  }

  const auto unclipped = renderer.measure(
      "no ellipsis mode keeps this paragraph", 18.0f, FontWeight::Normal, 80.0f, 1, TextAlign::Start, {},
      TextEllipsize::None);
  if (unclipped.lineCount != 1 || unclipped.width <= 0.0f) {
    std::cerr << "no-ellipsis paragraph mode failed layout\n";
    return 1;
  }


  const auto forcedLtr = renderer.measure(
      "direction", 18.0f, FontWeight::Normal, 240.0f, 1, TextAlign::Start, {}, TextEllipsize::None,
      ParagraphDirection::Ltr);
  const auto forcedRtl = renderer.measure(
      "direction", 18.0f, FontWeight::Normal, 240.0f, 1, TextAlign::Start, {}, TextEllipsize::None,
      ParagraphDirection::Rtl);
  if (forcedRtl.inkLeft <= forcedLtr.inkLeft + 20.0f) {
    std::cerr << "explicit RTL paragraph direction did not move leading text to the right edge\n";
    return 1;
  }
  return 0;
}
