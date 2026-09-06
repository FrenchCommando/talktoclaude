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

// Asks Windows 11 to keep the icon in the taskbar corner instead of the
// overflow. There is no API; the per-icon choice lives in the registry
// under HKCU\Control Panel\NotifyIconSettings, keyed by exe path, and
// Explorer creates the entry shortly after the icon is first added. Returns
// true once the entry exists and IsPromoted is set; call again later if
// false. Workaround, not a contract: the fallback is Settings >
// Personalization > Taskbar > Other system tray icons.
bool promote();

}  // namespace Tray
