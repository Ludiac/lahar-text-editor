module;

#include <SDL3/SDL.h>

export module vulkan_app:InputHandler;

import std;

import :TextEditor;

// Represents the two primary input modes for the application.
enum class InputMode {
  NORMAL, // For navigation and commands (Vim-like)
  INSERT  // For typing text
};

export class InputHandler {
public:
  /**
   * @brief Constructs the input handler.
   * @param editor A reference to the main text editor model to be modified by input.
   */
  InputHandler(SDL_Window *window) : m_window(window) {
    // We start in NORMAL mode, so text input events are not needed initially.
    // This is the default state for SDL, but we call it explicitly for clarity.
    SDL_StopTextInput(m_window);
  }

  /**
   * @brief Main entry point for processing SDL events.
   * This should be called from the application's main event loop for each polled event.
   * @param event The SDL_Event to process.
   */
  void handleEvent(const SDL_Event &event) {
    // As per the guide, we use SDL_EVENT_KEY_DOWN for command-like actions.
    if (event.type == SDL_EVENT_KEY_DOWN) {
      handleKeyDown(event.key);
    }
    // For actual text entry, SDL_EVENT_TEXT_INPUT is the correct, modern approach.
    // It correctly handles different keyboard layouts, dead keys, and IMEs.
    else if (event.type == SDL_EVENT_TEXT_INPUT && m_mode == InputMode::INSERT) {
      // --- INTEGRATION POINT ---
      // This is where you would call your TextEditor to insert the typed characters.
      // For example: m_editor.insertText(event.text.text);
      std::println("Text input: {}", event.text.text);
    }
  }

  /**
   * @brief Returns the current input mode.
   * Useful for displaying status in the UI or altering other application logic.
   * @return The current InputMode.
   */
  [[nodiscard]] InputMode getMode() const { return m_mode; }

  void setSDLwindow(SDL_Window *window) { m_window = window; };

private:
  /**
   * @brief Handles all key down events, delegating to mode-specific handlers.
   * @param keyEvent The SDL_KeyboardEvent from the event loop.
   */
  void handleKeyDown(const SDL_KeyboardEvent &keyEvent) {
    // Per SDL3 best practices, we should ignore key repeat events for
    // single-shot actions like changing modes or triggering commands.
    if (keyEvent.repeat != 0) {
      return;
    }

    // The ESCAPE key is a global hotkey to always return to NORMAL mode.
    if (keyEvent.key == SDLK_ESCAPE) {
      setMode(InputMode::NORMAL);
      return;
    }

    // Delegate to the handler for the current mode.
    switch (m_mode) {
    case InputMode::NORMAL:
      handleNormalMode(keyEvent);
      break;
    case InputMode::INSERT:
      handleInsertMode(keyEvent);
      break;
    }
  }

  /**
   * @brief Handles key presses in NORMAL mode (Vim-like commands).
   * @param keyEvent The SDL_KeyboardEvent from the event loop.
   */
  void handleNormalMode(const SDL_KeyboardEvent &keyEvent) {
    // For Vim-like bindings, we use SDL_Keycode (key). This binds to the
    // character on the key ('h', 'j', 'k', 'l'), which is the expected behavior
    // for this type of control scheme, regardless of physical keyboard layout.
    switch (keyEvent.key) {
    // 'i' switches to INSERT mode.
    case SDLK_I:
      // Example of modifier key usage. A real implementation might have
      // Shift+I insert at the beginning of the line.
      if (keyEvent.key & SDL_KMOD_SHIFT) {
        std::println("Normal mode: Shift+'i' pressed (e.g., insert at line start)");
        // TODO: m_editor.moveToLineStart(); setMode(InputMode::INSERT);
        setMode(InputMode::INSERT);
      } else {
        setMode(InputMode::INSERT);
      }
      break;

    // --- VIM-LIKE TRAVERSAL (STUBS) ---
    case SDLK_H:
      // TODO: m_editor.moveCursorLeft();
      std::println("Normal mode: 'h' pressed (left)");
      break;
    case SDLK_L:
      // TODO: m_editor.moveCursorRight();
      std::println("Normal mode: 'l' pressed (right)");
      break;
    case SDLK_K:
      // TODO: m_editor.moveCursorUp();
      std::println("Normal mode: 'k' pressed (up)");
      break;
    case SDLK_J:
      // TODO: m_editor.moveCursorDown();
      std::println("Normal mode: 'j' pressed (down)");
      break;
    default:
      break;
    }
  }

  /**
   * @brief Handles special key presses in INSERT mode (e.g., Backspace, Enter).
   * Regular character input is handled by SDL_EVENT_TEXT_INPUT.
   * @param keyEvent The SDL_KeyboardEvent from the event loop.
   */
  void handleInsertMode(const SDL_KeyboardEvent &keyEvent) {
    // Check for modifier keys.
    const SDL_Keymod MOD = keyEvent.mod;

    switch (keyEvent.key) {
    case SDLK_BACKSPACE:
      // Ctrl+Backspace is a common shortcut for deleting a whole word.
      if (MOD & SDL_KMOD_CTRL) {
        // TODO: m_editor.deleteWordBackward();
        std::println("Insert mode: Ctrl+Backspace pressed (delete word)");
      } else {
        // TODO: m_editor.backspace();
        std::println("Insert mode: Backspace pressed (delete character)");
      }
      break;
    case SDLK_RETURN:
      // TODO: m_editor.newline();
      std::println("Insert mode: Return pressed");
      break;
    // Arrow keys could also be handled here for cursor movement within insert mode.
    default:
      break;
    }
  }

  /**
   * @brief Safely changes the input mode and manages SDL's text input state.
   * @param newMode The mode to switch to.
   */
  void setMode(InputMode newMode) {
    if (m_mode == newMode) {
      return; // No change.
    }

    m_mode = newMode;

    if (m_mode == InputMode::INSERT) {
      std::println("Switching to INSERT mode.");
      // Tell SDL to start sending SDL_EVENT_TEXT_INPUT events.
      SDL_StartTextInput(m_window);
    } else {
      std::println("Switching to NORMAL mode.");
      // Tell SDL to stop sending SDL_EVENT_TEXT_INPUT events.
      SDL_StopTextInput(m_window);
    }
  }

  SDL_Window *m_window;
  TextEditor *m_editor{nullptr};
  InputMode m_mode{InputMode::NORMAL};
};
