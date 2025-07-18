module;

#include "primitive_types.hpp"

export module vulkan_app:TextArea;

import std;
/**
 * @class TextEditor
 * @brief An optimized text editor model integrating a Piece Table with line management.
 *
 * This class directly manages the text content using a piece table and, crucially,
 * maintains a cache of line-start offsets. This avoids the need to reconstruct the
 * entire document string to find line breaks after every edit. It provides an
 * efficient way to retrieve visible lines for rendering and supports a basic
 * undo/redo mechanism.
 */
class TextEditor {
private:
  // Represents which buffer a piece belongs to.
  enum class BufferType : u8 { ORIGINAL, ADD };

  // A "piece" descriptor, representing a span of text in one of the buffers.
  struct Piece {
    BufferType buffer; // Which buffer this piece points to.
    size_t start;      // The starting index in the buffer.
    size_t length;     // The length of the span in the buffer.
  };

  using PieceList = std::list<Piece>;

  // Represents a snapshot of the editor's state for undo/redo.
  struct HistoryState {
    PieceList pieces;
    size_t length;
    std::vector<size_t> lineStarts; // Also save the line cache
  };

public:
  /**
   * @brief Constructs a TextEditor with initial content.
   * @param initial_content The starting text for the editor.
   */
  explicit TextEditor(std::string_view initial_content = "") : m_length(initial_content.length()) {
    m_original_buffer = std::make_unique<std::string>(initial_content);
    m_add_buffer = std::make_unique<std::string>();

    Piece initialPiece = {
        .buffer = BufferType::ORIGINAL, .start = 0, .length = m_original_buffer->length()};
    m_pieces.push_back(initialPiece);

    rebuildLineCache(); // Initial line calculation
    saveStateForUndo();
  }

  // --- Core Editing Operations ---

  /**
   * @brief Inserts text at a given character position.
   * @param pos The character position to insert at.
   * @param text The text to insert.
   */
  void insert(size_t pos, std::string_view text) {
    if (text.empty() || pos > m_length) {
      return;
    }
    saveStateForUndo();

    // 1. Add the new text to the 'add' buffer
    size_t addedStart = m_add_buffer->length();
    m_add_buffer->append(text);
    Piece newPiece = {.buffer = BufferType::ADD, .start = addedStart, .length = text.length()};

    // 2. Find the piece where the insertion occurs
    auto [iter, offset] = findPiece(pos);

    // 3. Insert the new piece and split the existing piece if necessary
    if (offset == 0) { // Insert at the start of a piece
      m_pieces.insert(iter, newPiece);
    } else if (offset == iter->length) { // Insert at the end of a piece
      m_pieces.insert(std::next(iter), newPiece);
    } else { // Insert in the middle, requiring a split
      Piece &currentPiece = *iter;
      Piece rightPart = {.buffer = currentPiece.buffer,
                         .start = currentPiece.start + offset,
                         .length = currentPiece.length - offset};
      currentPiece.length = offset; // This is now the left part

      auto nextIter = std::next(iter);
      nextIter = m_pieces.insert(nextIter, newPiece);
      m_pieces.insert(nextIter, rightPart);
    }

    m_length += text.length();

    // 4. OPTIMIZATION: Incrementally update the line cache
    updateLineCacheForInsert(pos, text);
  }

  /**
   * @brief Deletes a range of text.
   * @param pos The starting character position.
   * @param length The number of characters to delete.
   */
  void remove(size_t pos, size_t length) {
    if (length == 0 || pos >= m_length) {
      return;
    }
    saveStateForUndo();

    size_t endPos = pos + length;
    auto [start_iter, start_offset] = findPiece(pos);
    auto [end_iter, end_offset] = findPiece(endPos);

    // 1. OPTIMIZATION: Incrementally update line cache BEFORE deleting pieces
    updateLineCacheForDelete(pos, length);

    // 2. Trim or split the boundary pieces and erase the middle pieces
    auto firstAffected = start_iter;
    auto lastAffected = end_iter;

    if (start_iter == end_iter) { // Deletion within a single piece
      if (start_offset == 0 && end_offset == start_iter->length) {
        // Deleting the whole piece
        firstAffected = m_pieces.erase(start_iter);
      } else {
        // Split the piece into two parts (left and right of the deleted section)
        Piece &p = *start_iter;
        Piece rightPart = {
            .buffer = p.buffer, .start = p.start + end_offset, .length = p.length - end_offset};
        p.length = start_offset; // Left part
        if (p.length == 0) {
          firstAffected = m_pieces.erase(start_iter);
        } else {
          firstAffected = std::next(start_iter);
        }
        if (rightPart.length > 0) {
          m_pieces.insert(firstAffected, rightPart);
        }
      }
    } else {
      // Deletion spans multiple pieces
      // Trim the start piece
      start_iter->length = start_offset;
      if (start_iter->length == 0) {
        start_iter = m_pieces.erase(start_iter);
      } else {
        start_iter = std::next(start_iter);
      }

      // Erase all full pieces in between
      while (start_iter != end_iter) {
        start_iter = m_pieces.erase(start_iter);
      }

      // Trim the end piece
      end_iter->start += end_offset;
      end_iter->length -= end_offset;
      if (end_iter->length == 0) {
        m_pieces.erase(end_iter);
      }
    }

    m_length -= length;
  }

  // --- Undo/Redo ---

  void undo() {
    if (m_undo_stack.size() > 1) { // Keep at least one state
      m_redo_stack.push(m_undo_stack.top());
      m_undo_stack.pop();
      restoreState(m_undo_stack.top());
    }
  }

  void redo() {
    if (!m_redo_stack.empty()) {
      restoreState(m_redo_stack.top());
      m_undo_stack.push(m_redo_stack.top());
      m_redo_stack.pop();
    }
  }

  // --- Data Retrieval ---

  /**
   * @brief Gets the content of a specific line.
   * @param line_index The zero-based index of the line to retrieve.
   * @return The text of the line, without the newline character.
   */
  [[nodiscard]] std::string getLine(size_t lineIndex) const {
    if (lineIndex >= m_line_starts.size()) {
      return "";
    }

    size_t startPos = m_line_starts[lineIndex];
    size_t endPos =
        (lineIndex + 1 < m_line_starts.size()) ? m_line_starts[lineIndex + 1] : m_length;

    // The length of the line content, excluding the potential newline char
    size_t length = endPos - startPos;
    if (length > 0 && charAt(endPos - 1) == '\n') {
      length--;
    }

    return getTextInRange(startPos, length);
  }

  /**
   * @brief Gets all visible lines as a vector of strings.
   * @param first_visible_line The starting line index.
   * @param num_lines The number of lines to retrieve.
   * @return A vector of strings, one for each visible line.
   */
  [[nodiscard]] std::vector<std::string> getVisibleLines(size_t firstVisibleLine,
                                                         size_t numLines) const {
    std::vector<std::string> lines;
    if (m_line_starts.empty()) {
      return lines;
    }

    size_t endLine = std::min(firstVisibleLine + numLines, m_line_starts.size());
    lines.reserve(endLine - firstVisibleLine);

    for (size_t i = firstVisibleLine; i < endLine; ++i) {
      lines.push_back(getLine(i));
    }
    return lines;
  }

  /**
   * @brief Retrieves the entire document as a single string.
   * @note This is an expensive operation. Use for saving, not for display updates.
   */
  [[nodiscard]] std::string toString() const { return getTextInRange(0, m_length); }

  [[nodiscard]] size_t length() const { return m_length; }
  [[nodiscard]] size_t lineCount() const { return m_line_starts.size(); }

private:
  // --- Internal Helper Methods ---

  /**
   * @brief Finds the piece and offset corresponding to a character position.
   */
  std::pair<PieceList::iterator, size_t> findPiece(size_t pos) {
    size_t currentPos = 0;
    for (auto it = m_pieces.begin(); it != m_pieces.end(); ++it) {
      if (currentPos + it->length >= pos) {
        // Returns a mutable iterator
        return {it, pos - currentPos};
      }
      currentPos += it->length;
    }
    return {m_pieces.end(), 0};
  }

  // Const version for use in methods like charAt() or toString()
  [[nodiscard]] std::pair<PieceList::const_iterator, size_t> findPiece(size_t pos) const {
    size_t currentPos = 0;
    for (auto it = m_pieces.cbegin(); it != m_pieces.cend(); ++it) {
      if (currentPos + it->length >= pos) {
        // Returns a read-only iterator
        return {it, pos - currentPos};
      }
      currentPos += it->length;
    }
    return {m_pieces.cend(), 0};
  }

  /**
   * @brief Reconstructs the line start cache from scratch.
   * @note Should only be used for initialization or major state changes.
   */
  void rebuildLineCache() {
    m_line_starts.clear();
    m_line_starts.push_back(0); // Line 0 always starts at position 0

    size_t currentPos = 0;
    for (const auto &piece : m_pieces) {
      const std::string *buffer =
          (piece.buffer == BufferType::ORIGINAL) ? m_original_buffer.get() : m_add_buffer.get();
      for (size_t i = 0; i < piece.length; ++i) {
        if ((*buffer)[piece.start + i] == '\n') {
          if (currentPos + i + 1 < m_length) {
            m_line_starts.push_back(currentPos + i + 1);
          }
        }
      }
      currentPos += piece.length;
    }
  }

  /**
   * @brief Incrementally updates the line cache after an insertion.
   */
  void updateLineCacheForInsert(size_t pos, std::string_view text) {
    // Find which line the insertion happened on
    auto it = std::ranges::upper_bound(m_line_starts, pos);
    size_t lineIndex = std::distance(m_line_starts.begin(), it) - 1;

    // Shift all subsequent line starts
    for (size_t i = lineIndex + 1; i < m_line_starts.size(); ++i) {
      m_line_starts[i] += text.length();
    }

    // Find new newlines in the inserted text
    std::vector<size_t> newLinesInInsert;
    for (size_t i = 0; i < text.length(); ++i) {
      if (text[i] == '\n') {
        if (pos + i + 1 < m_length) {
          newLinesInInsert.push_back(pos + i + 1);
        }
      }
    }

    // Insert new line starts into the cache
    if (!newLinesInInsert.empty()) {
      m_line_starts.insert(m_line_starts.begin() + lineIndex + 1, newLinesInInsert.begin(),
                           newLinesInInsert.end());
    }
  }

  /**
   * @brief Incrementally updates the line cache after a deletion.
   */
  void updateLineCacheForDelete(size_t pos, size_t length) {
    // Find the start and end lines affected by the deletion
    auto startIt = std::ranges::upper_bound(m_line_starts, pos) - 1;
    auto endIt = std::ranges::upper_bound(m_line_starts, pos + length) - 1;

    size_t firstLineIdx = std::distance(m_line_starts.begin(), startIt);
    size_t lastLineIdx = std::distance(m_line_starts.begin(), endIt);

    // Remove line starts that were inside the deleted range
    if (lastLineIdx > firstLineIdx) {
      m_line_starts.erase(startIt + 1, endIt + 1);
    }

    // Shift all subsequent line starts
    for (size_t i = firstLineIdx + 1; i < m_line_starts.size(); ++i) {
      m_line_starts[i] -= length;
    }
  }

  /**
   * @brief Retrieves a substring of the document.
   */
  [[nodiscard]] std::string getTextInRange(size_t pos, size_t length) const {
    if (pos + length > m_length) {
      length = m_length - pos;
    }

    std::string result;
    result.reserve(length);

    size_t currentPos = 0;
    for (const auto &piece : m_pieces) {
      if (result.length() == length) {
        break;
      }

      size_t pieceEndPos = currentPos + piece.length;

      // Check if this piece overlaps with the requested range
      if (pos < pieceEndPos && currentPos < pos + length) {
        size_t copyStartInPiece = (pos > currentPos) ? (pos - currentPos) : 0;
        size_t copyEndInPiece =
            (pos + length < pieceEndPos) ? (pos + length - currentPos) : piece.length;
        size_t copyLen = copyEndInPiece - copyStartInPiece;

        const std::string *buffer =
            (piece.buffer == BufferType::ORIGINAL) ? m_original_buffer.get() : m_add_buffer.get();
        result.append(buffer->data() + piece.start + copyStartInPiece, copyLen);
      }
      currentPos += piece.length;
    }
    return result;
  }

  /**
   * @brief Gets the character at a specific position.
   */
  [[nodiscard]] char charAt(size_t pos) const {
    if (pos >= m_length) {
      return '\0';
    }
    auto [iter, offset] = findPiece(pos);
    const std::string *buffer =
        (iter->buffer == BufferType::ORIGINAL) ? m_original_buffer.get() : m_add_buffer.get();
    return (*buffer)[iter->start + offset];
  }

  void saveStateForUndo() {
    m_undo_stack.push({m_pieces, m_length, m_line_starts});
    // Clear redo stack on new action
    while (!m_redo_stack.empty()) {
      m_redo_stack.pop();
    }
  }

  void restoreState(const HistoryState &state) {
    m_pieces = state.pieces;
    m_length = state.length;
    m_line_starts = state.lineStarts;
  }

  // --- Member Variables ---

  // Piece Table data
  std::unique_ptr<std::string> m_original_buffer;
  std::unique_ptr<std::string> m_add_buffer;
  PieceList m_pieces;
  usize m_length = 0;

  // Line cache for efficient display
  std::vector<size_t> m_line_starts;

  // Undo/Redo stacks
  std::stack<HistoryState, std::vector<HistoryState>> m_undo_stack;
  std::stack<HistoryState, std::vector<HistoryState>> m_redo_stack;
};
