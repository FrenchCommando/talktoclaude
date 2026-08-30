#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>

// Listens for the media Play/Pause button via the System Media Transport
// Controls (SMTC) API. This is the layer a paired Bluetooth device's
// (e.g. Pixel Buds) AVRCP play/pause command actually goes through on
// Windows — it does NOT surface as a WH_KEYBOARD_LL keyboard event, so a
// keyboard hook can't see it (confirmed: a real keyboard's media key does
// show up as VK_MEDIA_PLAY_PAUSE, the earbuds' AVRCP press does not).
//
// To receive SMTC button presses we have to make this process look like an
// active "now playing" media app — Windows otherwise routes the button to
// whichever app currently owns that role (e.g. a video player). We do that
// by pointing a MediaPlayer at a silent looping WAV and marking the SMTC
// PlaybackStatus as Playing.
//
// That claim is not permanent: Windows hands the session to whichever app
// most recently *started* playing, so anything else that plays (a YouTube
// tab, Spotify) takes the button away and our silent loop — playing
// continuously since startup, never restarting — doesn't get it back. There
// is no API to ask who owns the session, and focus has nothing to do with
// it. So the silent playback is restarted on a timer, unconditionally, which
// keeps the button with this app for as long as it runs — the deliberate cost
// being that the buds can't control anything else meanwhile. Ctrl+Alt+V
// forces a re-claim immediately. See kReclaimInterval in trigger.cpp.
//
// Each press toggles recording; the callback is invoked with `true` to mean
// "start" and `false` to mean "stop".
class Trigger {
public:
    using ToggleCallback = std::function<void(bool starting)>;

    explicit Trigger(ToggleCallback callback);
    ~Trigger();

    // Starts silent playback (to claim the SMTC session) and pumps a
    // message loop so WinRT callbacks can be dispatched. Blocks until
    // stop() is called (from another thread) or the process receives
    // WM_QUIT. Run this on its own thread.
    void run();

    void stop();

    // Re-claims the SMTC session after another app has taken it. Called on a
    // timer while running, and by a global hotkey. `announce` logs to the
    // console as well as the file. Safe to call at any time.
    void reclaim(bool announce);

private:
    ToggleCallback callback_;
    bool recording_ = false;
    unsigned long threadId_ = 0;
    // SetTimer with a null window ignores the id you give it and returns a
    // generated one, which is what WM_TIMER's wParam carries. Spelled as
    // uintptr_t rather than UINT_PTR so this header stays free of windows.h,
    // the same reason threadId_ above is a plain unsigned long.
    std::uintptr_t reclaimTimerId_ = 0;

    winrt::Windows::Media::Playback::MediaPlayer mediaPlayer_{nullptr};
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream wavStream_{nullptr};
    winrt::event_token buttonPressedToken_{};
    winrt::event_token mediaFailedToken_{};

    // Last owner reported by Windows.Media.Control, so a re-claim only logs
    // when the button actually changes hands.
    std::string lastSessionOwner_;

    // A re-claim tears the player down and builds a new one; the silent WAV
    // stream outlives both.
    void startSession();
    void endSession();
    void reportSessionOwner();

    void onButtonPressed(
        winrt::Windows::Media::SystemMediaTransportControls const& sender,
        winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const& args);
};
