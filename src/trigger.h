#pragma once

#include <functional>

// Listens globally for the media Play/Pause key — which is what a paired
// Bluetooth device's (e.g. Pixel Buds) play/pause button shows up as on
// Windows via AVRCP. Each press toggles recording; the callback is invoked
// with `true` to mean "start" and `false` to mean "stop".
//
// A regular keyboard's Play/Pause media key also triggers this, which is
// convenient for testing without the earbuds.
class Trigger {
public:
    using ToggleCallback = std::function<void(bool starting)>;

    explicit Trigger(ToggleCallback callback);
    ~Trigger();

    // Installs the low-level keyboard hook and pumps the message loop.
    // Blocks until stop() is called (from another thread) or the process
    // receives WM_QUIT. Run this on its own thread.
    void run();

    void stop();

private:
    ToggleCallback callback_;
    bool recording_ = false;
    void* hookHandle_ = nullptr;
    unsigned long threadId_ = 0;

    static long __stdcall lowLevelKeyboardProc(int code, unsigned long long wParam, long long lParam);
    static Trigger* instance_; // hooks are process-global; single instance assumed.
};
