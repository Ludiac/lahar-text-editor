module;

#include "primitive_types.hpp"

export module vulkan_app:TextEditor;

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

    // 5. Update cursor if necessary
    if (m_cursor_pos >= pos) {
      m_cursor_pos += text.length();
    }
    m_desired_col = getCursorLineCol().second;
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

    // 3. Update cursor if it was in the deleted range
    if (m_cursor_pos > pos) {
      m_cursor_pos = (m_cursor_pos > pos + length) ? m_cursor_pos - length : pos;
    }
    m_desired_col = getCursorLineCol().second;
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
  [[nodiscard]] size_t lineCount() const {
    return m_line_starts.empty() ? 1 : m_line_starts.size();
  }
  [[nodiscard]] size_t getCursorPosition() const { return m_cursor_pos; }

  // --- Cursor and High-Level Editing ---

  void moveCursor(size_t pos) {
    m_cursor_pos = std::min(pos, m_length);
    m_desired_col = getCursorLineCol().second;
  }

  void moveCursorLeft() {
    if (m_cursor_pos > 0) {
      m_cursor_pos--;
      m_desired_col = getCursorLineCol().second;
    }
  }

  void moveCursorRight() {
    if (m_cursor_pos < m_length) {
      m_cursor_pos++;
      m_desired_col = getCursorLineCol().second;
    }
  }

  void moveCursorUp() {
    auto [line, col] = getCursorLineCol();
    if (line > 0) {
      size_t prev_line_idx = line - 1;
      std::string prev_line = getLine(prev_line_idx);
      size_t new_col = std::min((size_t)prev_line.length(), m_desired_col);
      m_cursor_pos = m_line_starts[prev_line_idx] + new_col;
    }
  }

  void moveCursorDown() {
    auto [line, col] = getCursorLineCol();
    if (line + 1 < lineCount()) {
      size_t next_line_idx = line + 1;
      std::string next_line = getLine(next_line_idx);
      size_t new_col = std::min((size_t)next_line.length(), m_desired_col);
      m_cursor_pos = m_line_starts[next_line_idx] + new_col;
    }
  }

  void insert(std::string_view text) {
    insert(m_cursor_pos, text);
    // Position is updated by the other insert overload
  }

  void backspace() {
    if (m_cursor_pos > 0) {
      remove(m_cursor_pos - 1, 1);
      // remove() already handles cursor update
    }
  }

  void newline() { insert(m_cursor_pos, "\n"); }

  [[nodiscard]] std::pair<size_t, size_t> getCursorLineCol() const {
    if (m_line_starts.empty()) {
      return {0, 0};
    }
    // Find the line the cursor is on using a reverse iterator or lower_bound
    auto it = std::ranges::upper_bound(m_line_starts, m_cursor_pos);
    size_t lineIndex =
        (it == m_line_starts.begin()) ? 0 : std::distance(m_line_starts.begin(), it) - 1;

    // Calculate the column
    size_t lineStartPos = m_line_starts[lineIndex];
    size_t colIndex = m_cursor_pos - lineStartPos;

    return {lineIndex, colIndex};
  }

  /**
   * @brief Creates a TextEditor instance by loading content from a file.
   * @param filePath The path to the file to load.
   * @return An std::optional<TextEditor> containing the editor on success, or std::nullopt on
   * failure.
   */
  [[nodiscard]] static std::expected<TextEditor, std::string>
  loadFromFile(const std::filesystem::path &filePath) {
    if (!std::filesystem::exists(filePath)) {
      // If the file doesn't exist, create an empty editor.
      // You could also return std::nullopt here if you prefer to treat it as an error.
      return TextEditor("");
    }

    std::ifstream inFile(filePath, std::ios::binary);
    if (!inFile) {
      return std::unexpected(
          std::format("Error: Could not open file for reading: {}", filePath.string()));
    }

    // Efficiently read the entire file into a string
    std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());

    return TextEditor(content);
  }

  [[nodiscard]] std::expected<void, std::string>
  saveToFile(const std::filesystem::path &filePath) const {
    std::ofstream outFile(filePath, std::ios::binary | std::ios::trunc); // Truncate to overwrite
    if (!outFile) {
      return std::unexpected(
          std::format("Error: Could not open file for writing: {}", filePath.string()));
    }

    // Iterate through the pieces and write their contents directly to the file
    // This is much more efficient than calling toString() first.
    for (const auto &piece : m_pieces) {
      const std::string *buffer =
          (piece.buffer == BufferType::ORIGINAL) ? m_original_buffer.get() : m_add_buffer.get();
      if (!outFile.write(buffer->data() + piece.start, piece.length)) {
        return std::unexpected(
            std::format("Error: Could not write to a file: {}", filePath.string()));
      }
    }
    return {};
  }

private:
  // --- Internal Helper Methods ---

  /**
   * @brief Finds the piece and offset corresponding to a character position.
   */
  std::pair<PieceList::iterator, size_t> findPiece(size_t pos) {
    size_t currentPos = 0;
    for (auto it = m_pieces.begin(); it != m_pieces.end(); ++it) {
      if (currentPos + it->length > pos || (currentPos + it->length == pos && it->length > 0)) {
        return {it, pos - currentPos};
      }
      currentPos += it->length;
    }
    // If pos is at the very end of the text, find the last piece.
    if (pos == m_length && !m_pieces.empty()) {
      auto last_piece = std::prev(m_pieces.end());
      return {last_piece, last_piece->length};
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
    size_t scan_pos = 0;
    for (char c : text) {
      if (c == '\n') {
        if (pos + scan_pos + 1 < m_length) {
          newLinesInInsert.push_back(pos + scan_pos + 1);
        }
      }
      scan_pos++;
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
    if (m_line_starts.empty())
      return;
    // Find the start and end lines affected by the deletion
    auto startIt = std::ranges::upper_bound(m_line_starts, pos);
    if (startIt != m_line_starts.begin())
      --startIt;

    auto endIt = std::ranges::upper_bound(m_line_starts, pos + length);
    if (endIt != m_line_starts.begin())
      --endIt;

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
    if (pos >= m_length)
      return "";
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
    if (iter == m_pieces.cend())
      return '\0';
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
  usize m_cursor_pos = 0;
  usize m_desired_col = 0; // Remembers the column for vertical movement

  // Line cache for efficient display
  std::vector<size_t> m_line_starts;

  // Undo/Redo stacks
  std::stack<HistoryState, std::vector<HistoryState>> m_undo_stack;
  std::stack<HistoryState, std::vector<HistoryState>> m_redo_stack;
};
