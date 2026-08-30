#include "trigger.h"

#include <cstdint>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Storage.Streams.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "logging.h"

using namespace winrt;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Storage::Streams;

namespace {

// Ctrl+Alt+V re-claims the SMTC session when another app has taken the media
// button. Registered process-wide, so it works whatever has focus.
constexpr int kReclaimHotkeyId = 1;
constexpr UINT kReclaimHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kReclaimHotkeyVk = 'V';

// ...and it's re-claimed on a timer anyway, so the button belongs to this app
// for as long as it's running. The cost is deliberate: while talktoclaude is
// open the buds can't pause YouTube, because both can't own the session.
constexpr UINT kReclaimIntervalMs = 3000;

// A tiny (~0.1s, 8kHz mono) silent WAV, looped. Its only job is to make
// MediaPlayer legitimately "playing" so this process claims the SMTC
// session and future AVRCP button presses route here instead of whatever
// app previously owned it.
std::vector<uint8_t> buildSilentWav() {
    constexpr uint32_t sampleRate = 8000;
    constexpr uint32_t numSamples = sampleRate / 10; // 0.1s
    constexpr uint32_t dataSize = numSamples * sizeof(int16_t);
    constexpr uint32_t riffSize = 36 + dataSize;

    std::vector<uint8_t> wav(44 + dataSize, 0);
    auto put = [&](size_t offset, const char* bytes, size_t n) {
        memcpy(wav.data() + offset, bytes, n);
    };
    auto putU32 = [&](size_t offset, uint32_t v) { memcpy(wav.data() + offset, &v, 4); };
    auto putU16 = [&](size_t offset, uint16_t v) { memcpy(wav.data() + offset, &v, 2); };

    put(0, "RIFF", 4);
    putU32(4, riffSize);
    put(8, "WAVE", 4);
    put(12, "fmt ", 4);
    putU32(16, 16);          // fmt chunk size
    putU16(20, 1);           // PCM
    putU16(22, 1);           // mono
    putU32(24, sampleRate);
    putU32(28, sampleRate * sizeof(int16_t)); // byte rate
    putU16(32, sizeof(int16_t));              // block align
    putU16(34, 16);          // bits per sample
    put(36, "data", 4);
    putU32(40, dataSize);
    // Remaining bytes (the actual samples) are already zero-initialized = silence.
    return wav;
}

} // namespace

Trigger::Trigger(ToggleCallback callback) : callback_(std::move(callback)) {}

Trigger::~Trigger() {
    stop();
}

void Trigger::onButtonPressed(
    SystemMediaTransportControls const&,
    SystemMediaTransportControlsButtonPressedEventArgs const& args) {
    const auto button = args.Button();
    if (button != SystemMediaTransportControlsButton::Play &&
        button != SystemMediaTransportControlsButton::Pause) {
        return;
    }

    recording_ = !recording_;
    if (callback_) callback_(recording_);
}

void Trigger::run() {
    // AudioCapture already CoInitializeEx's this thread as MTA; match that
    // apartment type here or WinRT's own init throws RPC_E_CHANGED_MODE.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    threadId_ = GetCurrentThreadId();

    const std::vector<uint8_t> wavBytes = buildSilentWav();

    InMemoryRandomAccessStream stream;
    DataWriter writer(stream);
    writer.WriteBytes(
        array_view<uint8_t const>(wavBytes.data(), wavBytes.data() + wavBytes.size()));
    writer.StoreAsync().get();
    writer.DetachStream();
    stream.Seek(0);

    wavStream_ = stream;
    startSession();

    // Thread-bound hotkey: WM_HOTKEY arrives in this loop, not a window proc.
    if (RegisterHotKey(nullptr, kReclaimHotkeyId, kReclaimHotkeyMods, kReclaimHotkeyVk)) {
        Log::info("Media button re-claimed every %us; Ctrl+Alt+V forces it now.\n",
                  kReclaimIntervalMs / 1000);
    } else {
        Log::error("[trigger] couldn't register Ctrl+Alt+V (already taken?)\n");
    }

    reclaimTimerId_ = SetTimer(nullptr, 0, kReclaimIntervalMs, nullptr);
    if (reclaimTimerId_ == 0) Log::error("[trigger] SetTimer failed; no auto re-claim\n");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == kReclaimHotkeyId) {
            reclaim(true);
            continue;
        }
        if (msg.message == WM_TIMER && msg.wParam == reclaimTimerId_) {
            // File only: every 3s, and the console is the dictation output.
            reclaim(false);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(nullptr, reclaimTimerId_);
    UnregisterHotKey(nullptr, kReclaimHotkeyId);
    endSession();
}

// Builds a brand-new MediaPlayer and starts it playing. A fresh player is
// what actually claims the SMTC session: pausing and re-playing the existing
// one coalesces into no state transition at all, so Windows never sees a new
// "started playing" and leaves the session with whatever is already playing.
void Trigger::startSession() {
    wavStream_.Seek(0);
    mediaPlayer_ = MediaPlayer();
    mediaPlayer_.Source(MediaSource::CreateFromStream(wavStream_, L"audio/wav"));
    mediaPlayer_.IsLoopingEnabled(true);
    // Not 0.0: a silent session looks like nothing playing to Windows, and it
    // is skipped when routing the hardware media button. Inaudible instead.
    mediaPlayer_.Volume(0.001);

    auto smtc = mediaPlayer_.SystemMediaTransportControls();
    smtc.IsEnabled(true);
    smtc.IsPlayEnabled(true);
    smtc.IsPauseEnabled(true);
    buttonPressedToken_ = smtc.ButtonPressed({this, &Trigger::onButtonPressed});

    mediaPlayer_.Play();
    smtc.PlaybackStatus(MediaPlaybackStatus::Playing);
}

void Trigger::endSession() {
    if (!mediaPlayer_) return;
    mediaPlayer_.SystemMediaTransportControls().ButtonPressed(buttonPressedToken_);
    buttonPressedToken_ = {};
    mediaPlayer_.Pause();
    mediaPlayer_ = nullptr;
}

void Trigger::reclaim(bool announce) {
    if (!wavStream_) return;
    endSession();
    startSession();
    if (announce) {
        Log::info("[media button re-claimed]\n");
    } else {
        // Every few seconds — only worth having when reading the log later.
        Log::fileOnly("[media button re-claimed]\n");
    }
}

void Trigger::stop() {
    if (threadId_ != 0) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        threadId_ = 0;
    }
}
