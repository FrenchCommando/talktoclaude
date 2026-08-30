#pragma once

#include <string>

namespace TextInjector {

// Types `text` into whatever window currently has keyboard focus, via
// SendInput. Works app-independently (unlike WM_CHAR to a specific HWND),
// which is what makes this work with a terminal running Claude Code.
void typeText(const std::string& utf8Text);

} // namespace TextInjector
