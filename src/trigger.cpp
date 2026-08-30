#include "trigger.h"

#include <cstdint>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Storage.Streams.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace winrt;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Storage::Streams;

namespace {

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

    mediaPlayer_ = MediaPlayer();
    mediaPlayer_.Source(MediaSource::CreateFromStream(stream, L"audio/wav"));
    mediaPlayer_.IsLoopingEnabled(true);
    mediaPlayer_.Volume(0.0);

    auto smtc = mediaPlayer_.SystemMediaTransportControls();
    smtc.IsEnabled(true);
    smtc.IsPlayEnabled(true);
    smtc.IsPauseEnabled(true);
    buttonPressedToken_ = smtc.ButtonPressed({this, &Trigger::onButtonPressed});

    mediaPlayer_.Play();
    smtc.PlaybackStatus(MediaPlaybackStatus::Playing);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    smtc.ButtonPressed(buttonPressedToken_);
    mediaPlayer_ = nullptr;
}

void Trigger::stop() {
    if (threadId_ != 0) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        threadId_ = 0;
    }
}
