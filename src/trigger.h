#pragma once

#include <functional>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Playback.h>

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

private:
    ToggleCallback callback_;
    bool recording_ = false;
    unsigned long threadId_ = 0;

    winrt::Windows::Media::Playback::MediaPlayer mediaPlayer_{nullptr};
    winrt::event_token buttonPressedToken_{};

    void onButtonPressed(
        winrt::Windows::Media::SystemMediaTransportControls const& sender,
        winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const& args);
};
