#include "audio_capture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "logging.h"

#define WIN32_LEAN_AND_MEAN
// windows.h defines min/max as macros, which breaks any std::max( call in
// this file. We want the <algorithm> versions.
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <audioclient.h>
#include <propsys.h>
#include <avrt.h>

namespace {

constexpr int kTargetSampleRate = 16000;

// Auto-stop tuning. The threshold is a judgment value: real speech through
// the buds' HFP mic measured peak ~0.03, so 0.01 sits 3x under speech while
// clearing the noise floor. Trailing silence and the hard cap are UX
// choices, not measurements.
constexpr float kSpeechThreshold = 0.01f;
constexpr uint64_t kTrailingSilenceMs = 1500;
constexpr uint64_t kMaxUtteranceMs = 30000;

// Silence kept either side of the speech after trimming. Enough that a soft
// onset or a trailing fricative isn't clipped, short enough that what's left
// can't form a decode window of its own.
constexpr size_t kKeepLeadSamples = kTargetSampleRate * 300 / 1000;
constexpr size_t kKeepTailSamples = kTargetSampleRate * 250 / 1000;

// Drop the silence either side of the speech.
//
// The auto-stop only fires after kTrailingSilenceMs, so every utterance ends
// with ~1.5s of silence, and whisper splits a capture into decode windows:
// the speech becomes one segment and that trailing silence becomes a second.
// Asked to decode ~1.5s of nothing the model doesn't emit nothing - it
// hallucinates, and what it most often hallucinates is the sentence it just
// decoded, which is how one spoken phrase gets typed twice ([LAPTOP]
// 2026-09-05: segment 0 [0..400] and segment 1 [400..600] identical over a
// 5.4s capture). whisper.cpp already clears the decoder's prompt history
// before a short final window, so this is not prompt carryover - the window
// simply shouldn't exist. Trimming removes it, and cuts decode time besides.
void trimSilence(std::vector<float>& audio) {
    const auto isLoud = [](float sample) { return std::fabs(sample) > kSpeechThreshold; };
    const auto firstLoud = std::find_if(audio.begin(), audio.end(), isLoud);
    if (firstLoud == audio.end()) return;  // No speech at all; main.cpp gates that.
    const auto lastLoud = std::find_if(audio.rbegin(), audio.rend(), isLoud).base();

    const size_t speechStart = static_cast<size_t>(firstLoud - audio.begin());
    const size_t speechEnd = static_cast<size_t>(lastLoud - audio.begin());
    const size_t begin = speechStart - std::min(kKeepLeadSamples, speechStart);
    const size_t end = std::min(audio.size(), speechEnd + kKeepTailSamples);

    // Erase the tail first: trimming the head would invalidate `end`.
    audio.erase(audio.begin() + end, audio.end());
    audio.erase(audio.begin(), audio.begin() + begin);
}

// PKEY_Device_FriendlyName and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out
// instead of pulled in from <functiondiscoverykeys_devpkey.h>/<ksmedia.h>:
// those headers only *declare* the symbols unless INITGUID is defined first,
// and defining it would instantiate every GUID in every header we include.
const PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string toUtf8(const wchar_t* wide) {
    if (!wide) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

// Shared mode hands out the mix-engine format, which is float32 on every
// modern Windows. The resampler below reinterpret_casts each packet to
// float*, though, so a PCM-int device would transcribe as garbage instead of
// failing outright. Check rather than assume.
bool isIeeeFloat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, kSubtypeIeeeFloat) != 0;
    }
    return false;
}

// Resampler state that has to survive between packets. Restarting the read
// position at every packet boundary quantises each packet to a whole number
// of output frames: at 48kHz the ratio is exactly 3 so nothing is lost, but
// at 44.1kHz (2.75625) every packet drops a fraction of a frame and restarts
// the interpolation phase, so the capture both drifts short and clicks at
// each seam. Carrying the position and the last input sample across removes
// both. Reset per utterance in start().
struct ResampleState {
    // Read position for the next output frame, relative to the start of the
    // next packet. Negative (down to -1) means that frame interpolates
    // between `previous` and the packet's first sample.
    double position = 0.0;
    float previous = 0.0f;
};

// Very small linear resampler + stereo->mono downmix. Good enough for
// speech dictation; not audiophile-grade, doesn't need to be.
void appendResampled(std::vector<float>& out, const float* interleaved,
                      UINT32 frames, WORD channels, DWORD srcRate,
                      ResampleState& state) {
    if (frames == 0) return;

    // Downmix to mono first.
    std::vector<float> mono(frames);
    for (UINT32 i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (WORD c = 0; c < channels; ++c) sum += interleaved[i * channels + c];
        mono[i] = sum / static_cast<float>(channels);
    }

    if (srcRate == static_cast<DWORD>(kTargetSampleRate)) {
        out.insert(out.end(), mono.begin(), mono.end());
        return;
    }

    // Linear-interpolation resample srcRate -> 16kHz. `sampleAt` reads the
    // packet with index -1 meaning the last sample of the previous one.
    const double ratio = static_cast<double>(srcRate) / kTargetSampleRate;
    const auto sampleAt = [&](double index) {
        return index < 0.0 ? state.previous : mono[static_cast<size_t>(index)];
    };
    // An output frame needs both of its neighbours, so stop at the last
    // position whose right-hand neighbour is still in this packet; whatever
    // is left over carries to the next one.
    for (double position = state.position; position < frames - 1.0; position += ratio) {
        const double floored = std::floor(position);
        const float frac = static_cast<float>(position - floored);
        out.push_back(sampleAt(floored) * (1.0f - frac) + sampleAt(floored + 1.0) * frac);
        state.position = position + ratio;
    }
    state.position -= frames;
    state.previous = mono[frames - 1];
}

size_t resampledFrameCount(UINT32 frames, DWORD srcRate) {
    if (srcRate == static_cast<DWORD>(kTargetSampleRate)) return frames;
    return static_cast<size_t>(frames / (static_cast<double>(srcRate) / kTargetSampleRate));
}

// WASAPI flags a packet SILENT rather than handing over a buffer of zeros.
// Append the zeros ourselves instead of dropping the packet: a muted stretch
// is still elapsed time, and skipping it splices the utterance together. It
// also keeps an all-silent capture distinguishable from a dead stream — one
// comes back full of zeros, the other comes back empty.
void appendSilence(std::vector<float>& out, UINT32 frames, DWORD srcRate) {
    if (frames == 0) return;
    out.insert(out.end(), resampledFrameCount(frames, srcRate), 0.0f);
}

} // namespace

struct AudioCapture::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    HANDLE captureEvent = nullptr;

    std::thread captureThread;
    std::atomic<bool> threadShouldRun{false};
    // True while an utterance is being kept. The capture loop also drains
    // packets while this is false rather than letting the shared buffer
    // overflow.
    std::atomic<bool> capturing{false};
    std::mutex bufferMutex;
    std::vector<float> buffer;
    // Guarded by bufferMutex: only the capture loop touches it, and only
    // under the lock, apart from the per-utterance reset in start().
    ResampleState resampleState;

    // Auto-stop state, reset per utterance. The utterance ends on trailing
    // silence after speech, or at the hard cap — never on a second button
    // press, which this hardware won't deliver while the mic is open.
    std::function<void()> utteranceEndCallback;
    std::atomic<bool> sawSpeech{false};
    std::atomic<bool> autoStopFired{false};
    std::atomic<uint64_t> lastLoudMs{0};
    std::atomic<uint64_t> recordStartMs{0};

    // Runs every capture-loop iteration (packet or 200ms timeout alike), so
    // the hard cap fires even if the stream goes dead.
    void maybeAutoStop() {
        if (!capturing.load() || autoStopFired.load()) return;
        const uint64_t now = GetTickCount64();
        const bool trailingSilence =
            sawSpeech.load() && now - lastLoudMs.load() >= kTrailingSilenceMs;
        const bool tooLong = now - recordStartMs.load() >= kMaxUtteranceMs;
        if (!trailingSilence && !tooLong) return;
        autoStopFired = true;
        Log::info(trailingSilence ? "[auto-stop: trailing silence]\n"
                                  : "[auto-stop: max utterance length]\n");
        if (utteranceEndCallback) utteranceEndCallback();
    }

    // Per-utterance counters. Without them "nothing was transcribed" can't be
    // told apart from "the device never delivered a packet".
    std::atomic<uint64_t> framesSeen{0};
    std::atomic<uint64_t> silentFrames{0};

    ~Impl() {
        if (captureClient) captureClient->Release();
        if (audioClient) audioClient->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (captureEvent) CloseHandle(captureEvent);
    }

    void captureLoop() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitializedHere = SUCCEEDED(hr);

        while (threadShouldRun.load()) {
            const DWORD waitResult = WaitForSingleObject(captureEvent, 200);
            maybeAutoStop();
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 packetLength = 0;
            captureClient->GetNextPacketSize(&packetLength);
            while (packetLength != 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    Log::fileOnly("[audio] GetBuffer failed: 0x%08lx\n", hr);
                    break;
                }

                if (framesAvailable > 0 && capturing.load()) {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        silentFrames += framesAvailable;
                        appendSilence(buffer, framesAvailable, mixFormat->nSamplesPerSec);
                        // The zeros bypass the resampler, so the sample it
                        // would interpolate from is now a zero too.
                        resampleState.previous = 0.0f;
                    } else {
                        appendResampled(buffer, reinterpret_cast<float*>(data),
                                         framesAvailable, mixFormat->nChannels,
                                         mixFormat->nSamplesPerSec, resampleState);
                        // Track speech for auto-stop; raw interleaved
                        // samples are fine for a threshold test.
                        const float* samples = reinterpret_cast<float*>(data);
                        const size_t count =
                            static_cast<size_t>(framesAvailable) * mixFormat->nChannels;
                        for (size_t i = 0; i < count; ++i) {
                            if (std::fabs(samples[i]) > kSpeechThreshold) {
                                lastLoudMs = GetTickCount64();
                                sawSpeech = true;
                                break;
                            }
                        }
                    }
                    framesSeen += framesAvailable;
                }

                captureClient->ReleaseBuffer(framesAvailable);
                captureClient->GetNextPacketSize(&packetLength);
            }
        }

        if (comInitializedHere) CoUninitialize();
    }
};

AudioCapture::AudioCapture() : impl_(new Impl()) {}

void AudioCapture::onUtteranceEnd(std::function<void()> callback) {
    impl_->utteranceEndCallback = std::move(callback);
}

AudioCapture::~AudioCapture() {
    if (impl_->audioClient) impl_->audioClient->Stop();
    impl_->threadShouldRun = false;
    if (impl_->captureThread.joinable()) impl_->captureThread.join();
    delete impl_;
}

bool AudioCapture::init() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Log::error("[audio] CoInitializeEx failed: 0x%08lx\n", hr);
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator),
                           reinterpret_cast<void**>(&impl_->enumerator));
    if (FAILED(hr)) { Log::error("[audio] enumerator failed\n"); return false; }

    hr = impl_->enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &impl_->device);
    if (FAILED(hr)) { Log::error("[audio] no default capture device\n"); return false; }

    // Which mic we got matters more than it sounds: Bluetooth earbuds expose
    // several endpoints for the same hardware and the default can be one that
    // isn't live, which looks exactly like a working-but-mute mic.
    IPropertyStore* properties = nullptr;
    if (SUCCEEDED(impl_->device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(kDeviceFriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
            Log::info("[audio] capture device: %s\n", toUtf8(name.pwszVal).c_str());
        }
        PropVariantClear(&name);
        properties->Release();
    }

    hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&impl_->audioClient));
    if (FAILED(hr)) { Log::error("[audio] Activate(IAudioClient) failed\n"); return false; }

    hr = impl_->audioClient->GetMixFormat(&impl_->mixFormat);
    if (FAILED(hr)) { Log::error("[audio] GetMixFormat failed\n"); return false; }

    Log::info("[audio] mix format: %lu Hz, %u ch, %u-bit (tag %u)\n",
              impl_->mixFormat->nSamplesPerSec, impl_->mixFormat->nChannels,
              impl_->mixFormat->wBitsPerSample, impl_->mixFormat->wFormatTag);
    if (!isIeeeFloat(impl_->mixFormat)) {
        Log::error("[audio] capture format is not 32-bit float; unsupported\n");
        return false;
    }

    const REFERENCE_TIME bufferDuration = 10 * 1000 * 1000; // 1 second, generous.
    hr = impl_->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         bufferDuration, 0, impl_->mixFormat, nullptr);
    if (FAILED(hr)) { Log::error("[audio] Initialize failed: 0x%08lx\n", hr); return false; }

    impl_->captureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    hr = impl_->audioClient->SetEventHandle(impl_->captureEvent);
    if (FAILED(hr)) { Log::error("[audio] SetEventHandle failed\n"); return false; }

    hr = impl_->audioClient->GetService(__uuidof(IAudioCaptureClient),
                                         reinterpret_cast<void**>(&impl_->captureClient));
    if (FAILED(hr)) { Log::error("[audio] GetService(capture) failed\n"); return false; }

    impl_->threadShouldRun = true;
    impl_->captureThread = std::thread([this] { impl_->captureLoop(); });

    return true;
}

void AudioCapture::start() {
    if (recording_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->bufferMutex);
        impl_->buffer.clear();
        impl_->resampleState = {};
    }
    impl_->framesSeen = 0;
    impl_->silentFrames = 0;
    lastPeak_ = 0.0f;
    impl_->sawSpeech = false;
    impl_->autoStopFired = false;
    impl_->recordStartMs = GetTickCount64();
    impl_->lastLoudMs = impl_->recordStartMs.load();

    const HRESULT hr = impl_->audioClient->Start();
    if (FAILED(hr)) {
        Log::error("[audio] IAudioClient::Start failed: 0x%08lx\n", hr);
        return;
    }
    impl_->capturing = true;
    recording_ = true;
}

float AudioCapture::speechThreshold() { return kSpeechThreshold; }

std::vector<float> AudioCapture::stop() {
    if (!recording_) return {};
    impl_->capturing = false;
    recording_ = false;
    const HRESULT hr = impl_->audioClient->Stop();
    if (FAILED(hr)) Log::error("[audio] IAudioClient::Stop failed: 0x%08lx\n", hr);

    // Stop() pauses the stream; it does not empty the endpoint buffer. The
    // capture loop only drains packets when the event signals, which it no
    // longer does, so whatever was queued at this instant survives until the
    // next Start() — and is then appended to the *next* utterance, up to the
    // 1s buffer. Reset() drops it. It can lose the race against a packet the
    // capture thread is still holding, so it's not guaranteed; log and carry
    // on rather than pretending otherwise.
    const HRESULT resetHr = impl_->audioClient->Reset();
    if (FAILED(resetHr)) Log::fileOnly("[audio] IAudioClient::Reset failed: 0x%08lx\n", resetHr);

    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    std::vector<float> result = std::move(impl_->buffer);
    impl_->buffer.clear();

    // Peak describes what the mic delivered, so measure it before trimming:
    // main.cpp reads it to tell a dead mic apart from an unspoken utterance,
    // and a trimmed capture is all speech by construction.
    for (const float sample : result) lastPeak_ = std::max(lastPeak_, std::fabs(sample));

    const double capturedSeconds = result.size() / static_cast<double>(kTargetSampleRate);
    trimSilence(result);

    Log::info("[captured %.1fs, kept %.1fs, peak %.4f, %llu frames from device "
              "(%llu flagged silent)]\n",
              capturedSeconds, result.size() / static_cast<double>(kTargetSampleRate),
              lastPeak_, static_cast<unsigned long long>(impl_->framesSeen.load()),
              static_cast<unsigned long long>(impl_->silentFrames.load()));
    return result;
}
