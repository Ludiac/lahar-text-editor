module;

#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:PieceTable;

import vulkan_hpp;
import std;

/**
 * @class PieceTable
 * @brief A data structure for efficient text editing.
 *
 * Implements the Piece Table algorithm, which allows for fast insertions and
 * deletions by manipulating a list of descriptors ("pieces") rather than

 * moving large blocks of memory. The text is stored in an original immutable
 * buffer and an append-only buffer for new additions.
 */
class PieceTable {
private:
  // Represents which buffer a piece belongs to.
  enum class BufferType { ORIGINAL, ADD };

  // A "piece" descriptor.
  struct Piece {
    BufferType buffer; // Which buffer this piece points to.
    size_t start;      // The starting index in the buffer.
    size_t length;     // The length of the span in the buffer.
  };

  // Use a std::list for pieces because we need stable iterators
  // and efficient insertions/deletions in the middle of the sequence.
  using PieceList = std::list<Piece>;

public:
  /**
   * @brief Constructs a PieceTable with an initial string.
   * @param initial_content The starting text.
   */
  explicit PieceTable(std::string_view initial_content) {
    m_original_buffer = std::make_unique<std::string>(initial_content);
    m_add_buffer = std::make_unique<std::string>();
    m_pieces.push_back({BufferType::ORIGINAL, 0, m_original_buffer->length()});
    m_length = initial_content.length();
  }

  /**
   * @brief Inserts text at a specific position.
   * @param pos The character position to insert at.
   * @param text The text to insert.
   */
  void insert(size_t pos, std::string_view text) {
    if (text.empty())
      return;

    // Append the new text to the "add" buffer.
    size_t added_start = m_add_buffer->length();
    m_add_buffer->append(text);
    Piece new_piece = {BufferType::ADD, added_start, text.length()};

    // Find the piece and offset where the insertion occurs.
    auto [iter, offset] = find_piece(pos);

    // Case 1: Insertion at the exact start of a piece.
    if (offset == 0) {
      m_pieces.insert(iter, new_piece);
    }
    // Case 2: Insertion at the exact end of a piece.
    else if (offset == iter->length) {
      m_pieces.insert(std::next(iter), new_piece);
    }
    // Case 3: Insertion in the middle of a piece. Split the piece.
    else {
      Piece &current_piece = *iter;

      // Create the right part of the split piece.
      Piece right_part = {current_piece.buffer, current_piece.start + offset,
                          current_piece.length - offset};

      // The current piece becomes the left part.
      current_piece.length = offset;

      // Insert the new pieces into the list.
      auto next_iter = std::next(iter);
      next_iter = m_pieces.insert(next_iter, new_piece);
      m_pieces.insert(next_iter, right_part);
    }

    m_length += text.length();
  }

  /**
   * @brief Deletes a range of text.
   * @param pos The starting character position.
   * @param length The number of characters to delete.
   */
  void remove(size_t pos, size_t length) {
    if (length == 0 || pos >= m_length)
      return;

    auto [start_iter, start_offset] = find_piece(pos);
    auto [end_iter, end_offset] = find_piece(pos + length);

    // Erase all full pieces in the deletion range.
    auto current_iter = std::next(start_iter);
    while (current_iter != end_iter) {
      current_iter = m_pieces.erase(current_iter);
    }

    // Handle the boundary pieces.
    if (start_iter == end_iter) {
      // Deletion is within a single piece.
      start_iter->length -= length;
      // This requires a more complex split if we want to keep the piece clean.
      // For simplicity here, we can create two new pieces.
      if (start_offset > 0) {
        Piece right_part = {start_iter->buffer, start_iter->start + start_offset + length,
                            end_iter->length - end_offset};
        start_iter->length = start_offset;
        if (right_part.length > 0) {
          m_pieces.insert(std::next(start_iter), right_part);
        }
      } else { // Deletion at the start
        start_iter->start += length;
      }

    } else {
      // Deletion spans multiple pieces.
      start_iter->length = start_offset;
      end_iter->length -= end_offset;
      end_iter->start += end_offset;
    }

    // Cleanup empty pieces.
    m_pieces.remove_if([](const Piece &p) { return p.length == 0; });

    m_length -= length;
  }

  /**
   * @return The total length of the text.
   */
  size_t length() const { return m_length; }

  /**
   * @brief Retrieves the entire document as a single string.
   * @note This can be slow for very large documents. Use for saving.
   */
  std::string to_string() const {
    std::string result;
    result.reserve(m_length);
    for (const auto &piece : m_pieces) {
      if (piece.buffer == BufferType::ORIGINAL) {
        result.append(m_original_buffer->data() + piece.start, piece.length);
      } else {
        result.append(m_add_buffer->data() + piece.start, piece.length);
      }
    }
    return result;
  }

  /**
   * @brief An iterator-like class to traverse the text content.
   */
  class const_iterator {
    // Implementation details...
  };

private:
  /**
   * @brief Finds the piece and offset corresponding to a character position.
   * @param pos The global character position.
   * @return A pair containing the list iterator to the piece and the offset within it.
   */
  std::pair<PieceList::iterator, size_t> find_piece(size_t pos) {
    size_t current_pos = 0;
    for (auto it = m_pieces.begin(); it != m_pieces.end(); ++it) {
      if (current_pos + it->length > pos) {
        return {it, pos - current_pos};
      }
      current_pos += it->length;
    }
    // If pos == m_length, return the end iterator and offset 0
    return {m_pieces.end(), 0};
  }

  std::unique_ptr<std::string> m_original_buffer;
  std::unique_ptr<std::string> m_add_buffer;
  PieceList m_pieces;
  size_t m_length = 0;
};
