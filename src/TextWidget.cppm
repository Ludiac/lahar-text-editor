module;

#include "primitive_types.hpp"
#include <glm/glm.hpp>

export module vulkan_app:TextWidget;

import :TextEditor;
import :TextView;
import :UISystem;
import :TextSystem;
import :ui; // For Quad
import :text; // for Font

export class TextWidget {
private:
    TextEditor* m_editor;
    TextView m_view;
    Quad m_background;
    bool m_isActive = false;

public:
    TextWidget(TextEditor* editor, Font* font, Quad bg)
        : m_editor(editor), m_view(*editor, font), m_background(bg) {}

    void setFont(Font* font) {
        m_view.setFont(font);
    }

    TextEditor* getEditor() { return m_editor; }
    bool isActive() const { return m_isActive; }
    void setActive(bool active) { m_isActive = active; }


    void draw(UISystem& uiSystem, TextSystem& textSystem) {
        Font* font = m_view.getFont();
        if (!font || !m_editor) return;

        // Draw background
        uiSystem.queueQuad(m_background);

        // Draw text
        m_view.m_width = m_background.size.x;
        m_view.m_height = m_background.size.y;
        usize firstLine = m_view.getFirstVisibleLine();
        usize numLines = m_view.getVisibleLineCount();
        usize lastLine = std::min(firstLine + numLines, m_editor->lineCount());
        f64 currentLineYpos = m_background.position.y;

        const f64 POINT_SIZE = m_view.m_fontPointSize;
        const f64 FONT_UNIT_TO_PIXEL_SCALE =
            POINT_SIZE * (96.0 / 72.0 * 2) / font->atlasData.unitsPerEm;
        const f64 LINE_HEIGHT_PX =
            font->atlasData.lineHeight * FONT_UNIT_TO_PIXEL_SCALE;
        const f64 effectiveLineHeight = (LINE_HEIGHT_PX > 0) ? LINE_HEIGHT_PX : 38.0;

        for (usize i = firstLine; i < lastLine; ++i) {
          textSystem.queueText(font, m_editor->getLine(i),
                                 static_cast<u32>(m_view.m_fontPointSize), m_background.position.x,
                                 currentLineYpos, {1.0, 1.0, 1.0, 1.0});
          currentLineYpos += effectiveLineHeight;
        }

        // Draw Cursor
        if (m_isActive) {
            auto [cursorLine, cursorCol] = m_editor->getCursorLineCol();
            if (cursorLine >= firstLine && cursorLine < lastLine) {
                std::string lineToCursor = m_editor->getLine(cursorLine).substr(0, cursorCol);
                
                double cursorX = m_background.position.x;
                double textWidth = 0.0;
                const auto& metrics = font->atlasData;
                for (char c : lineToCursor) {
                    auto it = metrics.glyphs.find(static_cast<u32>(c));
                    if (it == metrics.glyphs.end()) it = metrics.glyphs.find(static_cast<u32>('?'));
                    if (it != metrics.glyphs.end()) {
                        textWidth += it->second.advance * FONT_UNIT_TO_PIXEL_SCALE;
                    }
                }
                cursorX += textWidth;

                double cursorY = m_background.position.y + (cursorLine - firstLine) * effectiveLineHeight;
                uiSystem.queueQuad({.position = {(float)cursorX, (float)cursorY}, .size = {2, (float)effectiveLineHeight}, .color = {1.0, 1.0, 1.0, 1.0}, .zLayer = 1.0});
            }
        }
    }
};
