#pragma once

#include <string>

// Notification-area icon. Owned by the trigger's hidden window: the icon
// needs an HWND to deliver clicks to, and that window already exists and is
// pumped by the trigger's message loop. This header stays free of windows.h;
// the window is passed as an opaque pointer.
namespace Tray {

enum class State { Idle, Listening, Transcribing, Busy };

constexpr unsigned kCallbackMessage = 0x8000 + 3;  // WM_APP + 3
constexpr unsigned kMenuOpenLogs = 1;
constexpr unsigned kMenuQuit = 2;

// Adds the icon. `window` receives kCallbackMessage for mouse events.
bool add(void* window);
void remove(void* window);

// State picks the icon colour; `tip` is the hover text (truncated to 127).
void set(void* window, State state, const std::string& tip);

// One-shot balloon. Used for the first-run model download.
void notify(void* window, const std::string& title, const std::string& text);

// Shows the right-click menu at the cursor and returns the chosen kMenu*
// id, or 0. Call from the window's thread.
unsigned showMenu(void* window);

}  // namespace Tray
