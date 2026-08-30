#include "audio_capture.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

namespace {

constexpr int kTargetSampleRate = 16000;

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
    double ratio = static_cast<double>(srcRate) / kTargetSampleRate;
    size_t outFrames = static_cast<size_t>(frames / ratio);
    size_t base = out.size();
    out.resize(base + outFrames);
    for (size_t i = 0; i < outFrames; ++i) {
        double srcPos = i * ratio;
        size_t i0 = static_cast<size_t>(srcPos);
        size_t i1 = (i0 + 1 < frames) ? i0 + 1 : i0;
        float frac = static_cast<float>(srcPos - i0);
        out[base + i] = mono[i0] * (1.0f - frac) + mono[i1] * frac;
    }
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
        bool comInitializedHere = SUCCEEDED(hr);

        while (threadShouldRun.load()) {
            DWORD waitResult = WaitForSingleObject(captureEvent, 200);
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 packetLength = 0;
            captureClient->GetNextPacketSize(&packetLength);
            while (packetLength != 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (framesAvailable > 0 && !(flags & AUDIOCLNT_BUFFERFLAGS_SILENT)) {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    appendResampled(buffer, reinterpret_cast<float*>(data),
                                     framesAvailable, mixFormat->nChannels,
                                     mixFormat->nSamplesPerSec);
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
        fprintf(stderr, "[audio] CoInitializeEx failed: 0x%08lx\n", hr);
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator),
                           reinterpret_cast<void**>(&impl_->enumerator));
    if (FAILED(hr)) { fprintf(stderr, "[audio] enumerator failed\n"); return false; }

    hr = impl_->enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &impl_->device);
    if (FAILED(hr)) { fprintf(stderr, "[audio] no default capture device\n"); return false; }

    hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&impl_->audioClient));
    if (FAILED(hr)) { fprintf(stderr, "[audio] Activate(IAudioClient) failed\n"); return false; }

    hr = impl_->audioClient->GetMixFormat(&impl_->mixFormat);
    if (FAILED(hr)) { fprintf(stderr, "[audio] GetMixFormat failed\n"); return false; }

    REFERENCE_TIME bufferDuration = 10 * 1000 * 1000; // 1 second, generous.
    hr = impl_->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         bufferDuration, 0, impl_->mixFormat, nullptr);
    if (FAILED(hr)) { fprintf(stderr, "[audio] Initialize failed: 0x%08lx\n", hr); return false; }

    impl_->captureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    hr = impl_->audioClient->SetEventHandle(impl_->captureEvent);
    if (FAILED(hr)) { fprintf(stderr, "[audio] SetEventHandle failed\n"); return false; }

    hr = impl_->audioClient->GetService(__uuidof(IAudioCaptureClient),
                                         reinterpret_cast<void**>(&impl_->captureClient));
    if (FAILED(hr)) { fprintf(stderr, "[audio] GetService(capture) failed\n"); return false; }

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
    impl_->audioClient->Start();
    recording_ = true;
}

std::vector<float> AudioCapture::stop() {
    if (!recording_) return {};
    impl_->audioClient->Stop();
    recording_ = false;

    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    std::vector<float> result = std::move(impl_->buffer);
    impl_->buffer.clear();
    return result;
}
