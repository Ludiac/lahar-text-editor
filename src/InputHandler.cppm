module;

#include <SDL3/SDL.h>

export module vulkan_app:InputHandler;

import std;

import :TextWidget; // Import TextWidget
import :TwoDEngine;

// Represents the two primary input modes for the application.
enum class InputMode {
  NORMAL, // For navigation and commands (Vim-like)
  INSERT  // For typing text
};

export class InputHandler {
public:
  /**
   * @brief Constructs the input handler.
   */
  InputHandler(SDL_Window *window, TwoDEngine *twoDEngine)
      : m_windowPtr(window), m_twoDEnginePtr(twoDEngine) {
    // We start in NORMAL mode, so text input events are not needed initially.
    SDL_StopTextInput(m_windowPtr);
  }

  /**
   * @brief Main entry point for processing SDL events.
   * This should be called from the application's main event loop for each polled event.
   * @param event The SDL_Event to process.
   */
  void handleEvent(const SDL_Event &event) {
    if (!m_twoDEnginePtr || m_twoDEnginePtr->getActiveWidget() == nullptr) {
      return; // Do nothing if no widget is active
    }

    if (event.type == SDL_EVENT_KEY_DOWN) {
      handleKeyDown(event.key);
    } else if (event.type == SDL_EVENT_TEXT_INPUT && m_mode == InputMode::INSERT) {
      TextView *view = m_twoDEnginePtr->getActiveWidget()->getActiveViewFORINPUTHANDLER();
      TextEditor *editor = view->getEditor();
      if (editor != nullptr) {
        editor->insert(event.text.text);
      }
    }
  }

  /**
   * @brief Returns the current input mode.
   */
  [[nodiscard]] InputMode getMode() const { return m_mode; }

  void setSDLwindow(SDL_Window *window) { m_windowPtr = window; };
  void set2DEngine(TwoDEngine *twoDEngine) { m_twoDEnginePtr = twoDEngine; };

private:
  /**
   * @brief Handles all key down events, delegating to mode-specific handlers.
   */
  void handleKeyDown(const SDL_KeyboardEvent &keyEvent) {
    if (keyEvent.repeat != 0) {
      // Allow repeats for navigation, deletion, and widget manipulation
      bool is_nav_key = (keyEvent.key == SDLK_UP || keyEvent.key == SDLK_DOWN ||
                         keyEvent.key == SDLK_LEFT || keyEvent.key == SDLK_RIGHT);

      if (!is_nav_key && keyEvent.key != SDLK_BACKSPACE) {
        return;
      }
    }

    if (keyEvent.key == SDLK_ESCAPE) {
      setMode(InputMode::NORMAL);
      return;
    }

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
   * @brief Handles key presses in NORMAL mode (Vim-like commands and widget manipulation).
   */
  void handleNormalMode(const SDL_KeyboardEvent &keyEvent) {
    TextView *view = m_twoDEnginePtr->getActiveWidget()->getActiveViewFORINPUTHANDLER();
    TextEditor *editor = view->getEditor();
    if (editor == nullptr) {
      return;
    }

    const SDL_Keymod modState = keyEvent.mod;
    const bool isCtrlPressed = (modState & SDL_KMOD_CTRL);

    constexpr float MOVE_SPEED = 10.0;
    constexpr int RESIZE_SPEED = 10;

    if (isCtrlPressed) {
      // --- WIDGET RESIZING AND FONT SCALING ---
      switch (keyEvent.key) {
      case SDLK_UP:
        view->changeSize(0, -RESIZE_SPEED);
        break;
      case SDLK_DOWN:
        view->changeSize(0, RESIZE_SPEED);
        break;
      case SDLK_LEFT:
        view->changeSize(-RESIZE_SPEED, 0);
        break;
      case SDLK_RIGHT:
        view->changeSize(RESIZE_SPEED, 0);
        break;
      case SDLK_MINUS:
        view->changeFontPointSize(-1.0);
        break;
      case SDLK_EQUALS: // Represents the '+' key
        view->changeFontPointSize(1.0);
        break;
      default:
        break;
      }
    } else {
      // --- EDITOR NAVIGATION AND WIDGET MOVEMENT ---
      switch (keyEvent.key) {
      case SDLK_I:
        setMode(InputMode::INSERT);
        break;
      // Widget Movement
      case SDLK_UP:
        m_twoDEnginePtr->getActiveWidget()->move(0, -MOVE_SPEED);
        break;
      case SDLK_DOWN:
        m_twoDEnginePtr->getActiveWidget()->move(0, MOVE_SPEED);
        break;
      case SDLK_LEFT:
        m_twoDEnginePtr->getActiveWidget()->move(-MOVE_SPEED, 0);
        break;
      case SDLK_RIGHT:
        m_twoDEnginePtr->getActiveWidget()->move(MOVE_SPEED, 0);
        break;
      // Editor Cursor Movement (VIM keys)
      case SDLK_H:
        editor->moveCursorLeft();
        break;
      case SDLK_L:
        editor->moveCursorRight();
        break;
      case SDLK_K:
        editor->moveCursorUp();
        break;
      case SDLK_J:
        editor->moveCursorDown();
        break;

      case SDLK_TAB:
        m_twoDEnginePtr->cycleActiveWidgetFocus();
        break;
      default:
        break;
      }
    }
  }

  /**
   * @brief Handles special key presses in INSERT mode.
   */
  void handleInsertMode(const SDL_KeyboardEvent &keyEvent) {
    TextEditor *editor =
        m_twoDEnginePtr->getActiveWidget()->getActiveViewFORINPUTHANDLER()->getEditor();
    if (editor == nullptr) {
      return;
    }

    switch (keyEvent.key) {
    case SDLK_BACKSPACE:
      editor->backspace();
      break;
    case SDLK_RETURN:
      editor->newline();
      break;
    // Arrow keys for cursor movement within insert mode.
    case SDLK_LEFT:
      editor->moveCursorLeft();
      break;
    case SDLK_RIGHT:
      editor->moveCursorRight();
      break;
    case SDLK_UP:
      editor->moveCursorUp();
      break;
    case SDLK_DOWN:
      editor->moveCursorDown();
      break;
    default:
      break;
    }
  }

  /**
   * @brief Safely changes the input mode and manages SDL's text input state.
   */
  void setMode(InputMode newMode) {
    if (m_mode == newMode)
      return;
    m_mode = newMode;

    if (m_mode == InputMode::INSERT) {
      std::println("Switching to INSERT mode.");
      SDL_StartTextInput(m_windowPtr);
    } else {
      std::println("Switching to NORMAL mode.");
      SDL_StopTextInput(m_windowPtr);
    }
  }

  TwoDEngine *m_twoDEnginePtr;
  SDL_Window *m_windowPtr;
  InputMode m_mode{InputMode::NORMAL};
};
