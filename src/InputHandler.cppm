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
  InputHandler(SDL_Window *window, TextEditor *editor) : m_window(window), m_editor(editor) {
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
      if (m_editor) {
        m_editor->insert(event.text.text);
      }
    }
  }

  /**
   * @brief Returns the current input mode.
   * Useful for displaying status in the UI or altering other application logic.
   * @return The current InputMode.
   */
  [[nodiscard]] InputMode getMode() const { return m_mode; }

  void setSDLwindow(SDL_Window *window) { m_window = window; };
  void setEditor(TextEditor *editor) { m_editor = editor; }

  [[nodiscard]] bool shouldCycleFocus() const { return m_cycleFocusRequested; }
  void resetCycleFocusFlag() { m_cycleFocusRequested = false; }

private:
  /**
   * @brief Handles all key down events, delegating to mode-specific handlers.
   * @param keyEvent The SDL_KeyboardEvent from the event loop.
   */
  void handleKeyDown(const SDL_KeyboardEvent &keyEvent) {
    // Per SDL3 best practices, we should ignore key repeat events for
    // single-shot actions like changing modes or triggering commands.
    if (keyEvent.repeat != 0) {
      // Allow repeats for navigation and deletion
      if (m_mode == InputMode::NORMAL &&
          (keyEvent.key == SDLK_H || keyEvent.key == SDLK_L || keyEvent.key == SDLK_K ||
           keyEvent.key == SDLK_J)) {
        // Process navigation repeats
      } else if (m_mode == InputMode::INSERT && keyEvent.key == SDLK_BACKSPACE) {
        // Process backspace repeats
      } else {
        return;
      }
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
    if (!m_editor)
      return;
    // For Vim-like bindings, we use SDL_Keycode (key). This binds to the
    // character on the key ('h', 'j', 'k', 'l'), which is the expected behavior
    // for this type of control scheme, regardless of physical keyboard layout.
    switch (keyEvent.key) {
    // 'i' switches to INSERT mode.
    case SDLK_I:
      setMode(InputMode::INSERT);
      break;

    // --- VIM-LIKE TRAVERSAL (STUBS) ---
    case SDLK_H:
      m_editor->moveCursorLeft();
      break;
    case SDLK_L:
      m_editor->moveCursorRight();
      break;
    case SDLK_K:
      // TODO: m_editor.moveCursorUp();
      std::println("Normal mode: 'k' pressed (up)");
      break;
    case SDLK_J:
      // TODO: m_editor.moveCursorDown();
      std::println("Normal mode: 'j' pressed (down)");
      break;
    case SDLK_TAB:
        m_cycleFocusRequested = true;
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
    if (!m_editor)
      return;

    // Check for modifier keys.
    const SDL_Keymod MOD = keyEvent.mod;

    switch (keyEvent.key) {
    case SDLK_BACKSPACE:
        // TODO: Handle Ctrl+Backspace for word deletion
        m_editor->backspace();
      break;
    case SDLK_RETURN:
      m_editor->newline();
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
  bool m_cycleFocusRequested = false;
};
