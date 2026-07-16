#include "ui/controls/markdown_view.h"

#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cstdint>
#include <charconv>
#include <md4c.h>
#include <memory>
#include <string>
#include <vector>

namespace {

  struct MdContext {
    MarkdownView* view = nullptr;
    float scale = 1.0f;
    std::string textBuf;
    std::vector<StyledTextRun> styledRuns;
    unsigned boldDepth = 0;
    unsigned italicDepth = 0;
    unsigned monospaceDepth = 0;
    unsigned underlineDepth = 0;
    unsigned strikeDepth = 0;
    int headingLevel = 0;
    bool inCodeBlock = false;
    bool inOrderedList = false;
    int listItemNumber = 0;
    std::vector<bool> listOrderedStack;
    std::vector<int> listNumberStack;
    bool inTable = false;
    bool inTableHeader = false;
    std::vector<std::string> tableRow;
    std::string tableCellBuf;
    std::vector<std::pair<std::vector<std::string>, bool>> tableRows;
    std::vector<std::size_t> tableColumnWidths;
  };

  void appendStyled(MdContext& ctx, std::string_view text) {
    if (text.empty()) return;
    StyledTextRun run{
        .text = std::string(text),
        .bold = ctx.boldDepth > 0,
        .italic = ctx.italicDepth > 0,
        .monospace = ctx.monospaceDepth > 0,
        .underline = ctx.underlineDepth > 0,
        .strikeThrough = ctx.strikeDepth > 0,
        .color = ctx.underlineDepth > 0 ? std::optional<Color>(colorForRole(ColorRole::Primary)) : std::nullopt,
    };
    if (!ctx.styledRuns.empty()) {
      auto previous = ctx.styledRuns.back();
      previous.text.clear();
      auto current = run;
      current.text.clear();
      if (previous == current) {
        ctx.styledRuns.back().text += run.text;
        ctx.textBuf += run.text;
        return;
      }
    }
    ctx.textBuf += run.text;
    ctx.styledRuns.push_back(std::move(run));
  }

  std::string decodeEntity(std::string_view entity) {
    if (entity == "&amp;") return "&";
    if (entity == "&lt;") return "<";
    if (entity == "&gt;") return ">";
    if (entity == "&quot;") return "\"";
    if (entity == "&apos;") return "'";
    if (entity.size() < 4 || entity[0] != '&' || entity[1] != '#' || entity.back() != ';') {
      return std::string(entity);
    }
    const bool hexadecimal = entity.size() > 4 && (entity[2] == 'x' || entity[2] == 'X');
    const auto digits = entity.substr(hexadecimal ? 3 : 2, entity.size() - (hexadecimal ? 4 : 3));
    std::uint32_t codepoint = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), codepoint, hexadecimal ? 16 : 10);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || codepoint > 0x10FFFF
        || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return std::string(entity);
    }
    std::string decoded;
    if (codepoint <= 0x7F) decoded.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7FF) {
      decoded.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      decoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
      decoded.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      decoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      decoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      decoded.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
      decoded.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      decoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      decoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return decoded;
  }

  constexpr int kWrapUnlimited = 500;

  std::unique_ptr<Label> makeMarkdownLabel(
      const std::string& text, float fontSize, float scale, ColorRole color, FontWeight weight = FontWeight::Normal,
      int maxLines = kWrapUnlimited, const std::vector<StyledTextRun>& runs = {}
  ) {
    auto label = ui::label({
        .text = text,
        .fontSize = fontSize * scale,
        .fontWeight = weight,
        .color = colorSpecFromRole(color),
        .maxLines = maxLines,
    });
    if (!runs.empty()) label->setStyledText(runs);
    return label;
  }

  void emitHeading(MdContext& ctx) {
    float fontSize = Style::fontSizeBody;
    switch (ctx.headingLevel) {
    case 1:
      fontSize = Style::fontSizeHeader;
      break;
    case 2:
      fontSize = Style::fontSizeTitle;
      break;
    case 3:
      fontSize = Style::fontSizeBody * 1.1f;
      break;
    default:
      break;
    }
    ctx.view->addChild(ui::row({.height = Style::spaceSm * ctx.scale}));
    ctx.view->addChild(
        makeMarkdownLabel(ctx.textBuf, fontSize, ctx.scale, ColorRole::OnSurface, FontWeight::Bold, 1, ctx.styledRuns)
    );
    ctx.view->addChild(ui::separator({.spacing = Style::spaceXs * ctx.scale * 0.5f}));
  }

  void emitParagraph(MdContext& ctx) {
    if (ctx.textBuf.empty()) {
      return;
    }
    auto label =
        makeMarkdownLabel(
            ctx.textBuf, Style::fontSizeBody, ctx.scale, ColorRole::OnSurface, FontWeight::Normal,
            kWrapUnlimited, ctx.styledRuns);
    ctx.view->trackWrappableLabel(label.get());
    ctx.view->addChild(std::move(label));
  }

  void emitCodeBlock(MdContext& ctx) {
    while (!ctx.textBuf.empty() && ctx.textBuf.back() == '\n') {
      ctx.textBuf.pop_back();
    }
    if (ctx.textBuf.empty()) {
      return;
    }
    const float pad = Style::spaceSm * ctx.scale;
    auto block = ui::column({
        .align = FlexAlign::Start,
        .padding = pad,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.5f),
        .radius = Style::scaledRadiusSm(ctx.scale),
        .fillWidth = true,
    });
    block->addChild(
        ui::label({
            .text = ctx.textBuf,
            .fontSize = Style::fontSizeCaption * ctx.scale,
            .fontFamily = std::string("monospace"),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = kWrapUnlimited,
            .textAlign = TextAlign::Start,
        })
    );
    ctx.view->addChild(std::move(block));
  }

  void emitTableRow(MdContext& ctx) {
    if (ctx.tableRow.empty()) {
      return;
    }
    if (ctx.tableColumnWidths.empty()) {
      ctx.tableColumnWidths.resize(ctx.tableRow.size(), 0);
    }
    for (std::size_t i = 0; i < ctx.tableRow.size() && i < ctx.tableColumnWidths.size(); ++i) {
      ctx.tableColumnWidths[i] = std::max(ctx.tableColumnWidths[i], ctx.tableRow[i].size());
    }
    ctx.tableRows.emplace_back(ctx.tableRow, ctx.inTableHeader);
    ctx.tableRow.clear();
  }

  void flushTable(MdContext& ctx) {
    if (ctx.tableRows.empty()) {
      return;
    }
    std::string block;
    for (const auto& [cells, isHeader] : ctx.tableRows) {
      std::string line;
      for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) {
          line += "  ";
        }
        line += cells[i];
        if (i < ctx.tableColumnWidths.size() && i + 1 < cells.size()) {
          const auto pad = ctx.tableColumnWidths[i] - cells[i].size();
          line.append(pad, ' ');
        }
      }
      if (!block.empty()) {
        block += '\n';
      }
      block += line;
    }
    auto label = ui::label({
        .text = block,
        .fontSize = Style::fontSizeCaption * ctx.scale,
        .fontFamily = std::string("monospace"),
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxLines = kWrapUnlimited,
        .textAlign = TextAlign::Start,
    });
    label->setFlexGrow(1.0f);
    ctx.view->addChild(std::move(label));
    ctx.tableRows.clear();
    ctx.tableColumnWidths.clear();
  }

  void emitListItem(MdContext& ctx) {
    if (ctx.textBuf.empty()) {
      return;
    }
    auto row = ui::row({.align = FlexAlign::Start, .gap = Style::spaceXs * ctx.scale});
    std::string bullet;
    if (!ctx.listOrderedStack.empty() && ctx.listOrderedStack.back()) {
      bullet = std::to_string(ctx.listItemNumber) + ".";
    } else {
      bullet = "•";
    }
    row->addChild(
        makeMarkdownLabel(bullet, Style::fontSizeBody, ctx.scale, ColorRole::OnSurfaceVariant, FontWeight::Normal)
    );
    auto textLabel =
        makeMarkdownLabel(
            ctx.textBuf, Style::fontSizeBody, ctx.scale, ColorRole::OnSurface, FontWeight::Normal,
            kWrapUnlimited, ctx.styledRuns);
    ctx.view->trackWrappableLabel(textLabel.get());
    textLabel->setFlexGrow(1.0f);
    row->addChild(std::move(textLabel));
    row->setFillWidth(true);
    const float indent = Style::spaceMd * ctx.scale * static_cast<float>(ctx.listOrderedStack.size() - 1);
    if (indent > 0.0f) {
      row->setPadding(0.0f, 0.0f, 0.0f, indent);
    }
    ctx.view->addChild(std::move(row));
  }

  int onEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    ctx->textBuf.clear();
    ctx->styledRuns.clear();
    switch (type) {
    case MD_BLOCK_H: {
      const auto* hd = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
      ctx->headingLevel = static_cast<int>(hd->level);
      break;
    }
    case MD_BLOCK_CODE:
      ctx->inCodeBlock = true;
      break;
    case MD_BLOCK_UL:
      ctx->listOrderedStack.push_back(false);
      ctx->listNumberStack.push_back(0);
      break;
    case MD_BLOCK_OL: {
      const auto* od = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
      ctx->listOrderedStack.push_back(true);
      ctx->listNumberStack.push_back(static_cast<int>(od->start) - 1);
      break;
    }
    case MD_BLOCK_LI:
      if (!ctx->listNumberStack.empty()) {
        ctx->listNumberStack.back()++;
        ctx->listItemNumber = ctx->listNumberStack.back();
      }
      ctx->textBuf.clear();
      break;
    case MD_BLOCK_TABLE:
      ctx->inTable = true;
      break;
    case MD_BLOCK_THEAD:
      ctx->inTableHeader = true;
      break;
    case MD_BLOCK_TBODY:
      ctx->inTableHeader = false;
      break;
    case MD_BLOCK_TD:
    case MD_BLOCK_TH:
      ctx->tableCellBuf.clear();
      break;
    default:
      break;
    }
    return 0;
  }

  int onLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    switch (type) {
    case MD_BLOCK_H:
      emitHeading(*ctx);
      ctx->headingLevel = 0;
      break;
    case MD_BLOCK_P:
      emitParagraph(*ctx);
      break;
    case MD_BLOCK_CODE:
      emitCodeBlock(*ctx);
      ctx->inCodeBlock = false;
      break;
    case MD_BLOCK_HR:
      ctx->view->addChild(ui::separator({.spacing = Style::spaceXs * ctx->scale}));
      break;
    case MD_BLOCK_LI:
      emitListItem(*ctx);
      break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      if (!ctx->listOrderedStack.empty()) {
        ctx->listOrderedStack.pop_back();
      }
      if (!ctx->listNumberStack.empty()) {
        ctx->listNumberStack.pop_back();
      }
      break;
    case MD_BLOCK_TABLE:
      flushTable(*ctx);
      ctx->inTable = false;
      break;
    case MD_BLOCK_THEAD:
      ctx->inTableHeader = false;
      break;
    case MD_BLOCK_TR:
      emitTableRow(*ctx);
      break;
    case MD_BLOCK_TD:
    case MD_BLOCK_TH:
      ctx->tableRow.push_back(ctx->tableCellBuf);
      ctx->tableCellBuf.clear();
      break;
    default:
      break;
    }
    if (!ctx->inTable) {
      ctx->textBuf.clear();
      ctx->styledRuns.clear();
    }
    return 0;
  }

  int onEnterSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    if (ctx->inCodeBlock) {
      return 0;
    }
    switch (type) {
    case MD_SPAN_STRONG:
      ++ctx->boldDepth;
      break;
    case MD_SPAN_EM:
      ++ctx->italicDepth;
      break;
    case MD_SPAN_CODE:
      ++ctx->monospaceDepth;
      break;
    case MD_SPAN_A:
      ++ctx->underlineDepth;
      break;
    case MD_SPAN_DEL:
      ++ctx->strikeDepth;
      break;
    default:
      break;
    }
    return 0;
  }

  int onLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    if (ctx->inCodeBlock) {
      return 0;
    }
    switch (type) {
    case MD_SPAN_STRONG:
      if (ctx->boldDepth > 0) --ctx->boldDepth;
      break;
    case MD_SPAN_EM:
      if (ctx->italicDepth > 0) --ctx->italicDepth;
      break;
    case MD_SPAN_CODE:
      if (ctx->monospaceDepth > 0) --ctx->monospaceDepth;
      break;
    case MD_SPAN_A:
      if (ctx->underlineDepth > 0) --ctx->underlineDepth;
      break;
    case MD_SPAN_DEL:
      if (ctx->strikeDepth > 0) --ctx->strikeDepth;
      break;
    default:
      break;
    }
    return 0;
  }

  int onText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    auto& buf = ctx->inTable ? ctx->tableCellBuf : ctx->textBuf;
    switch (type) {
    case MD_TEXT_NORMAL:
      if (ctx->inCodeBlock || ctx->inTable) {
        buf.append(text, size);
      } else {
        appendStyled(*ctx, std::string_view(text, size));
      }
      break;
    case MD_TEXT_CODE:
      if (ctx->inTable || ctx->inCodeBlock) buf.append(text, size);
      else appendStyled(*ctx, std::string_view(text, size));
      break;
    case MD_TEXT_SOFTBR:
      if (ctx->inTable || ctx->inCodeBlock) buf += ' ';
      else appendStyled(*ctx, " ");
      break;
    case MD_TEXT_BR:
      if (ctx->inTable || ctx->inCodeBlock) buf += '\n';
      else appendStyled(*ctx, "\n");
      break;
    case MD_TEXT_ENTITY:
      if (ctx->inTable || ctx->inCodeBlock) buf.append(text, size);
      else appendStyled(*ctx, decodeEntity(std::string_view(text, size)));
      break;
    default:
      buf.append(text, size);
      break;
    }
    return 0;
  }

} // namespace

MarkdownView::MarkdownView() {
  m_paletteConnection = paletteChanged().connect([this] {
    if (!m_markdown.empty()) setMarkdown(m_markdown, m_scale);
  });
}

void MarkdownView::setMarkdown(const std::string& markdown, float scale) {
  m_markdown = markdown;
  clear();
  m_scale = scale;
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm * scale);
  setFillWidth(true);

  MdContext ctx;
  ctx.view = this;
  ctx.scale = scale;

  MD_PARSER parser = {};
  parser.abi_version = 0;
  parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
  parser.enter_block = onEnterBlock;
  parser.leave_block = onLeaveBlock;
  parser.enter_span = onEnterSpan;
  parser.leave_span = onLeaveSpan;
  parser.text = onText;

  md_parse(markdown.c_str(), static_cast<MD_SIZE>(markdown.size()), &parser, &ctx);
}

void MarkdownView::clear() {
  m_wrappableLabels.clear();
  while (!children().empty()) {
    removeChild(children().back().get());
  }
}

void MarkdownView::doLayout(Renderer& renderer) {
  const float w = width();
  if (w > 0.0f) {
    for (Label* label : m_wrappableLabels) {
      label->setMaxWidth(w);
    }
  }
  Flex::doLayout(renderer);
}
