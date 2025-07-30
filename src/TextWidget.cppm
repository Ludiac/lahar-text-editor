module;

#include "primitive_types.hpp"
#include <glm/glm.hpp>

export module vulkan_app:TextWidget;

import :TextEditor;
import :TextView;
import :UISystem;
import :TextSystem;
import :ui;   // For Quad
import :text; // for Font

// CreateInfo class for TextWidget
struct TextWidgetCreateInfo {
  TextEditor *editor;
  Font *font;
  double fontPointSize;
  glm::vec2 position;
  vk::Extent2D size;
  glm::vec4 textColor;
  glm::vec4 bgColor;
};

// TextWidget class
class TextWidget {
private:
  TextEditor *m_editor;
  TextView m_view;
  glm::vec2 m_position;
  glm::vec4 m_bgColor;
  glm::vec4 m_textColor;
  bool m_bgActive{true};
  float m_zLayer{0.0};
  bool m_isActive{false};

public:
  TextWidget(const TextWidgetCreateInfo &ci)
      : m_editor(ci.editor), m_view(*ci.editor, ci.font, ci.fontPointSize, ci.size),
        m_position(ci.position), m_bgColor(ci.bgColor), m_textColor(ci.textColor) {}

  void setFont(Font *font) { m_view.setFont(font); }

  TextEditor *getEditor() { return m_editor; }
  [[nodiscard]] bool isActive() const { return m_isActive; }
  void setActive(bool active) { m_isActive = active; }

  void draw(UISystem &uiSystem, TextSystem &textSystem) {
    Font *font = m_view.getFont();
    if (!font || !m_editor) {
      return;
    }

    uiSystem.queueQuad(
        {.position = m_position, .size = m_view.size, .color = m_bgColor, .zLayer = m_zLayer});

    usize firstLine = m_view.getFirstVisibleLine();
    usize numLines = m_view.getVisibleLineCount();
    usize lastLine = std::min(firstLine + numLines, m_editor->lineCount());

    const f64 POINT_SIZE = m_view.fontPointSize;
    const f64 FONT_UNIT_TO_PIXEL_SCALE = POINT_SIZE * (96.0 / 72.0) / font->atlasData.unitsPerEm;
    const f64 LINE_HEIGHT_PX = font->atlasData.lineHeight * FONT_UNIT_TO_PIXEL_SCALE;

    f64 currentLineYpos = m_position.y + LINE_HEIGHT_PX;

    usize maxCol = m_view.maxColumnsInView();
    for (usize i = firstLine; i < lastLine; ++i) {
      const std::string &line = m_editor->getLine(i);
      // Only take the substring that is actually visible
      std::string visible = line.substr(0, maxCol);
      textSystem.queueText(font, visible, m_view.fontPointSize, m_position.x, currentLineYpos,
                           m_textColor); // color can be styled later
      currentLineYpos += LINE_HEIGHT_PX;
    }
    // Draw Cursor
    if (m_isActive) {
      auto [cursorLine, cursorCol] = m_editor->getCursorLineCol();
      if (cursorCol > maxCol) {
        return; // cursor is fully off-screen horizontally
      }
      if (cursorLine >= firstLine && cursorLine < lastLine) {
        std::string lineToCursor = m_editor->getLine(cursorLine).substr(0, cursorCol);

        double cursorX = m_position.x;
        double textWidth = 0.0;
        const auto &metrics = font->atlasData;
        for (char c : lineToCursor) {
          auto it = metrics.glyphs.find(static_cast<u32>(c));
          if (it == metrics.glyphs.end()) {
            it = metrics.glyphs.find(static_cast<u32>('?'));
          }
          if (it != metrics.glyphs.end()) {
            textWidth += it->second.advance * FONT_UNIT_TO_PIXEL_SCALE;
          }
        }
        cursorX += textWidth;

        double cursorY = m_position.y + ((cursorLine - firstLine) * LINE_HEIGHT_PX);
        uiSystem.queueQuad({.position = {(f32)cursorX, (f32)cursorY},
                            .size = {.width = 2, .height = static_cast<u32>(LINE_HEIGHT_PX)},
                            .color = m_textColor,
                            .zLayer = 0.0});
      }
    }
  }
};
