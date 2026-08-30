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

size_t resampledFrameCount(UINT32 frames, DWORD srcRate) {
    if (srcRate == static_cast<DWORD>(kTargetSampleRate)) return frames;
    return static_cast<size_t>(frames / (static_cast<double>(srcRate) / kTargetSampleRate));
}

// Very small linear resampler + stereo->mono downmix. Good enough for
// speech dictation; not audiophile-grade, doesn't need to be.
void appendResampled(std::vector<float>& out, const float* interleaved,
                      UINT32 frames, WORD channels, DWORD srcRate) {
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

    // Linear-interpolation resample srcRate -> 16kHz.
    const double ratio = static_cast<double>(srcRate) / kTargetSampleRate;
    const size_t outFrames = resampledFrameCount(frames, srcRate);
    const size_t base = out.size();
    out.resize(base + outFrames);
    for (size_t i = 0; i < outFrames; ++i) {
        const double srcPos = i * ratio;
        const size_t i0 = static_cast<size_t>(srcPos);
        const size_t i1 = (i0 + 1 < frames) ? i0 + 1 : i0;
        const float frac = static_cast<float>(srcPos - i0);
        out[base + i] = mono[i0] * (1.0f - frac) + mono[i1] * frac;
    }
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
    std::mutex bufferMutex;
    std::vector<float> buffer;

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

                if (framesAvailable > 0) {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        silentFrames += framesAvailable;
                        appendSilence(buffer, framesAvailable, mixFormat->nSamplesPerSec);
                    } else {
                        appendResampled(buffer, reinterpret_cast<float*>(data),
                                         framesAvailable, mixFormat->nChannels,
                                         mixFormat->nSamplesPerSec);
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
    }
    impl_->framesSeen = 0;
    impl_->silentFrames = 0;
    lastPeak_ = 0.0f;

    const HRESULT hr = impl_->audioClient->Start();
    if (FAILED(hr)) {
        Log::error("[audio] IAudioClient::Start failed: 0x%08lx\n", hr);
        return;
    }
    recording_ = true;
}

std::vector<float> AudioCapture::stop() {
    if (!recording_) return {};
    const HRESULT hr = impl_->audioClient->Stop();
    if (FAILED(hr)) Log::error("[audio] IAudioClient::Stop failed: 0x%08lx\n", hr);
    recording_ = false;

    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    std::vector<float> result = std::move(impl_->buffer);
    impl_->buffer.clear();

    for (const float sample : result) lastPeak_ = std::max(lastPeak_, std::fabs(sample));

    Log::info("[captured %.1fs, peak %.4f, %llu frames from device (%llu flagged silent)]\n",
              result.size() / static_cast<double>(kTargetSampleRate), lastPeak_,
              static_cast<unsigned long long>(impl_->framesSeen.load()),
              static_cast<unsigned long long>(impl_->silentFrames.load()));
    return result;
}
