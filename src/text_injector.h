#pragma once

#include <string>

namespace TextInjector {

// Opaque handle to a top-level window; an HWND, kept out of this header so
// callers don't need windows.h.
using Target = void*;

// The window that has focus right now. Take it at the starting press: that
// is the window the user was looking at when they decided to speak.
Target foregroundTarget();

// Types `text` into the focused window via SendInput, followed by Enter,
// but only if `target` still has focus. SendInput has no destination — it
// goes to whatever is in front when transcription *finishes* — and that has
// already submitted a stray prompt into an unrelated Claude session after
// focus moved mid-utterance. If focus has moved the transcript is logged
// and dropped, not typed somewhere else.
void typeText(const std::string& utf8Text, Target target);

} // namespace TextInjector
