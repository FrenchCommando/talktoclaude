#include "trigger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

Trigger* Trigger::instance_ = nullptr;

Trigger::Trigger(ToggleCallback callback) : callback_(std::move(callback)) {
    instance_ = this;
}

Trigger::~Trigger() {
    stop();
    instance_ = nullptr;
}

long __stdcall Trigger::lowLevelKeyboardProc(int code, unsigned long long wParam, long long lParam) {
    if (code == HC_ACTION && instance_) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_KEYDOWN && info->vkCode == VK_MEDIA_PLAY_PAUSE) {
            instance_->recording_ = !instance_->recording_;
            if (instance_->callback_) instance_->callback_(instance_->recording_);
            // Swallow the key so it doesn't also pause whatever media player
            // is in the background.
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, static_cast<WPARAM>(wParam), lParam);
}

void Trigger::run() {
    threadId_ = GetCurrentThreadId();
    hookHandle_ = SetWindowsHookExW(WH_KEYBOARD_LL,
                                     reinterpret_cast<HOOKPROC>(&Trigger::lowLevelKeyboardProc),
                                     GetModuleHandleW(nullptr), 0);
    if (!hookHandle_) return;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(static_cast<HHOOK>(hookHandle_));
    hookHandle_ = nullptr;
}

void Trigger::stop() {
    if (threadId_ != 0) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        threadId_ = 0;
    }
}
