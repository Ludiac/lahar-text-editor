module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <glm/glm.hpp>

export module vulkan_app:TextView;

import std;
import :TextEditor; // The module for TextEditor
import :text;       // The module for Font

// Represents the visual style for a range of text.
// For now, it only contains color, but can be expanded with font style (bold, italic), etc.
export struct TextStyle {
  glm::vec4 color{1.0, 1.0, 1.0, 1.0}; // Default to white
                                       // bool is_bold = false;
                                       // bool is_italic = false;
};

// Defines a styled range of text using character positions.
export struct StyledRange {
  usize start;
  usize length;
  TextStyle style;
} __attribute__((aligned(32)));

/**
 * @class TextView
 * @brief Manages the visible state and styling of a text document.
 *
 * Acts as the "View" in an MVC pattern. It does not own the text data itself
 * (that's the TextEditor's job), but it's responsible for:
 * 1.  Managing the scroll position to determine which lines are visible.
 * 2.  Storing and applying style information (like syntax highlighting) to ranges of text.
 * 3.  Providing the list of visible lines and their styles to a rendering system.
 */
export class TextView {
public:
  /**
   * @brief Constructs a TextView.
   * @param editor A reference to the TextEditor model.
   * @param font A reference to the Font used for metrics.
   */
  TextView(TextEditor &editor, Font &font, f64 pointSize = 36)
      : m_editor(editor), m_font(font), fontPointSize(pointSize) {}

  // --- Configuration ---

  /**
   * @brief Scrolls the view vertically by a number of lines.
   * @param delta_lines Positive to scroll down, negative to scroll up.
   */
  void scroll(i32 deltaLines) {
    if (deltaLines > 0) {
      usize maxFirstLine = m_editor.lineCount() > 0 ? m_editor.lineCount() - 1 : 0;
      m_first_visible_line = std::min(m_first_visible_line + (usize)deltaLines, maxFirstLine);
    } else if (deltaLines < 0) {
      usize scrollAmount = -deltaLines;
      m_first_visible_line =
          (m_first_visible_line > scrollAmount) ? m_first_visible_line - scrollAmount : 0;
    }
  }

  // --- Styling ---

  /**
   * @brief Applies a style to a specified range of characters.
   * @note This is a simple implementation. A real editor would need to handle
   *       merging and splitting overlapping style ranges.
   */
  void applyStyle(usize start, usize length, const TextStyle &style) {
    // For simplicity, we just add a new range.
    // A more robust implementation would merge this with existing ranges.
    m_styles.emplace_back(StyledRange{.start = start, .length = length, .style = style});
    // Keep styles sorted for efficient lookup.
    std::ranges::sort(m_styles, {}, &StyledRange::start);
  }

  /**
   * @brief Clears all custom styling.
   */
  void clearStyles() { m_styles.clear(); }

  /**
   * @brief Gets the style for a character at a specific position.
   * @return The applied style, or a default style if none is found.
   */
  [[nodiscard]] TextStyle getStyleAt(usize charPos) const {
    // Find the last style that starts at or before char_pos
    auto styleIt = std::ranges::upper_bound(m_styles, charPos, {}, &StyledRange::start);
    if (styleIt != m_styles.begin()) {
      --styleIt; // Move to the potential containing range
      // Check if the position is actually within this range
      if (charPos < styleIt->start + styleIt->length) {
        return styleIt->style;
      }
    }
    return m_default_style; // Return default if no style applies
  }

  // --- Data Retrieval for Rendering ---

  /**
   * @return The starting line index of the visible area.
   */
  [[nodiscard]] usize getFirstVisibleLine() const { return m_first_visible_line; }

  /**
   * @return The number of lines that can fit in the view's current height.
   */
  [[nodiscard]] usize getVisibleLineCount() const {
    if (m_font.atlasData.lineHeight <= 0) {
      return 20; // Fallback
    }
    // This is a simplified calculation. A real implementation would use font metrics.
    const f64 FONT_UNIT_TO_PIXEL_SCALE =
        fontPointSize * (96.0 / 72.0 * 4) / m_font.atlasData.unitsPerEm;
    const f64 LINE_HEIGHT_PX = m_font.atlasData.lineHeight * FONT_UNIT_TO_PIXEL_SCALE;

    return static_cast<usize>(height / LINE_HEIGHT_PX);
  }

  f64 width = 0.0;
  f64 height = 0.0;
  f64 fontPointSize = 36.0;

private:
  TextEditor &m_editor;
  Font &m_font;

  // Viewport state
  usize m_first_visible_line = 0;
  f32 m_horizontal_scroll_offset_px = 0.0;

  // Styling information
  TextStyle m_default_style;
  std::vector<StyledRange> m_styles;
};
