#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <winrt/Windows.Media.h>

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
// Each press toggles recording; the callback is invoked with `true` to mean
// "start" and `false` to mean "stop".
class Trigger {
public:
    using ToggleCallback = std::function<void(bool starting)>;

    explicit Trigger(ToggleCallback callback);
    ~Trigger();

    // Registers the SMTC session and pumps a message loop so WinRT
    // callbacks can be dispatched. Blocks until stop() is called (from
    // another thread) or the process receives WM_QUIT.
    void run();

    void stop();

    // Re-asserts the SMTC session after another app has taken it. Called on
    // a timer while running, and by a global hotkey. `announce` logs to the
    // console as well as the file.
    void reclaim(bool announce);

    // Ends the current recording as if the stop press had arrived. Safe to
    // call from any thread (posts to the trigger thread's message loop);
    // used by the capture side's silence auto-stop, since this hardware
    // delivers no button press while the mic is open.
    void requestStop();

private:
    ToggleCallback callback_;
    bool recording_ = false;
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
