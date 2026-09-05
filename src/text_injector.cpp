#include "text_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

#include "logging.h"

namespace {

// Title of a top-level window, for the log line that says where the
// transcript would have gone.
std::string windowTitle(HWND window) {
    wchar_t wide[128];
    const int len = window ? GetWindowTextW(window, wide, 128) : 0;
    if (len <= 0) return "(untitled)";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, len, out.data(), bytes, nullptr, nullptr);
    return out;
}

}  // namespace

namespace TextInjector {

Target foregroundTarget() { return GetForegroundWindow(); }

void typeText(const std::string& utf8Text, Target target) {
    if (utf8Text.empty()) return;

    const HWND foreground = GetForegroundWindow();
    if (foreground != static_cast<HWND>(target)) {
        Log::error("[inject] focus moved from \"%s\" to \"%s\" during the utterance; "
                   "transcript not typed\n",
                   windowTitle(static_cast<HWND>(target)).c_str(),
                   windowTitle(foreground).c_str());
        return;
    }

    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(),
                                            static_cast<int>(utf8Text.size()), nullptr, 0);
    if (wideLen <= 0) return;
    std::wstring wide(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), static_cast<int>(utf8Text.size()),
                         wide.data(), wideLen);

    // SendInput with KEYEVENTF_UNICODE handles arbitrary Unicode text without
    // needing to map characters to virtual-key codes / worry about layout.
    std::vector<INPUT> inputs;
    inputs.reserve(wide.size() * 2);
    for (wchar_t ch : wide) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = 0;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }

    // Follow with Enter so the transcript submits directly (e.g. into a
    // terminal prompt) instead of needing a manual keypress after each one.
    INPUT enterDown{};
    enterDown.type = INPUT_KEYBOARD;
    enterDown.ki.wVk = VK_RETURN;
    inputs.push_back(enterDown);

    INPUT enterUp = enterDown;
    enterUp.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(enterUp);

    // SendInput is blocked wholesale by a higher-integrity foreground window
    // (UAC prompts, some games) and stops at the first rejected event, so a
    // short count means the transcript was typed partially or not at all.
    // Silently losing it looks identical to whisper hearing nothing.
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        Log::error("[inject] only %u of %zu events were accepted (0x%08lx); the focused "
                   "window may not accept synthetic input\n",
                   sent, inputs.size(), GetLastError());
    }
}

} // namespace TextInjector
