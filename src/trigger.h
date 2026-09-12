#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Before the first C++/WinRT header anywhere in the translation unit:
// trigger.cpp's get_activation_factory<..., ISystemMediaTransportControls
// Interop> is a classic COM interface, and winrt/base.h only supports those
// if <unknwn.h> came first. MSVC happened to let it through; clang-cl (the
// arm64 build) enforces the static_assert.
#include <unknwn.h>
#include <winrt/Windows.Media.h>

#include "tray.h"

// Listens for the media Play/Pause button via the System Media Transport
// Controls (SMTC) API. This is the layer a Bluetooth headset's AVRCP
// play/pause command goes through on Windows — it does NOT surface as a
// keyboard event, and it is only delivered to an app that has registered a
// real SMTC media session. (Synthesized VK_MEDIA_PLAY_PAUSE keys are the
// exception: they reach ButtonPressed even without a registered session,
// which made the old implementation look healthier than it was.)
//
// The registration is done the canonical desktop way:
// ISystemMediaTransportControlsInterop::GetForWindow on a hidden window,
// with display metadata and PlaybackStatus = Playing. The previous
// implementation instead played an inaudible looping WAV through a
// MediaPlayer to claim the session; on `[DESKTOP]` that never registered a
// session at all (GetCurrentSession() stayed null while it "played"), so
// AVRCP presses had nothing to route to and vanished — the silent-audio
// hack and every workaround propping it up (volume 0.001, output pinning)
// are gone.
//
// Windows hands the button to whichever session most recently started
// playing, so anything else that plays (a YouTube tab, Spotify) takes it
// away. The status is re-asserted on a timer, and Ctrl+Alt+V forces it.
// Deliberate cost: while talktoclaude runs it wants the button, so the
// headset can't pause other apps' media.
//
// The trigger keeps no recording state of its own: a press and a stop
// request are reported as two events, and the owner (main.cpp) decides what
// each means from the capture's actual state. It used to keep a toggle flag
// alongside the capture's, and the two could disagree — a stop press landing
// just after the auto-stop had posted its own stop was processed as a fresh
// toggle and started a new recording. ButtonPressed fires on a WinRT
// threadpool thread, so presses are posted into run()'s message loop and
// both callbacks always run on the run() thread — serialized with each
// other and with however long a callback itself blocks (transcription).
class Trigger {
public:
    using Callback = std::function<void()>;

    // `onPress` for a Play/Pause press; `onStopRequest` for requestStop().
    // `logDir` is what the tray menu's "Open log folder" opens.
    Trigger(Callback onPress, Callback onStopRequest, std::string logDir);
    ~Trigger();

    // Creates the hidden window, the tray icon, and the SMTC registration.
    // Split from run() so the icon is up (showing "loading") while main
    // loads the model, which can take a first-run download. Same thread as
    // run(): the window, hotkey, and timer are bound to it.
    bool start();

    // Pumps the message loop so WinRT callbacks and tray clicks can be
    // dispatched. Blocks until stop() is called (from another thread), the
    // tray menu's Quit, or WM_QUIT.
    void run();

    void stop();

    // Tray icon colour and hover text. Call from the run() thread.
    void setState(Tray::State state, const std::string& tip);
    // One-shot balloon.
    void notify(const std::string& title, const std::string& text);

    // Tray callback plumbing, called from the window procedure.
    void onTrayEvent(unsigned event);

    // Re-asserts the SMTC session after another app has taken it. Called on
    // a timer while running, and by a global hotkey. `announce` writes a log
    // line; the timer path passes false so the log isn't a line every 3s.
    void reclaim(bool announce);

    // Delivers a stop request to the run() thread. Safe to call from any
    // thread (posts to the trigger thread's message loop); used by the
    // capture side's silence auto-stop, since this hardware delivers no
    // button press while the mic is open.
    void requestStop();

private:
    Callback onPress_;
    Callback onStopRequest_;
    std::string logDir_;
    void* window_ = nullptr;  // HWND, opaque here
    bool trayAdded_ = false;
    // Tray::promote() attempts left; Explorer creates the registry entry a
    // little after the icon appears, so it is retried on the 3s timer.
    int promoteAttempts_ = 10;
    unsigned long threadId_ = 0;
    // SetTimer with a null window ignores the id you give it and returns a
    // generated one, which is what WM_TIMER's wParam carries. Spelled as
    // uintptr_t rather than UINT_PTR so this header stays free of windows.h,
    // the same reason threadId_ above is a plain unsigned long.
    std::uintptr_t reclaimTimerId_ = 0;

    winrt::Windows::Media::SystemMediaTransportControls smtc_{nullptr};
    winrt::event_token buttonPressedToken_{};

    // Last owner reported by Windows.Media.Control, so the timer only logs
    // when the button actually changes hands.
    std::string lastSessionOwner_;

    void reportSessionOwner();

    void onButtonPressed(
        winrt::Windows::Media::SystemMediaTransportControls const& sender,
        winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const& args);
};
