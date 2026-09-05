#include "transcriber.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "logging.h"
#include "whisper.h"

namespace {

// Floor for the shrunken encoder context. Unlike the other figures here this
// one isn't derivable from the model — it's a judgement call about where
// accuracy starts to suffer, taken from whisper.cpp's --audio-ctx guidance
// rather than measured on this setup.
constexpr int kMinAudioCtx = 256;

std::string trim(const std::string& text) {
    const size_t start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return {};
    const size_t end = text.find_last_not_of(" \t\n\r");
    return text.substr(start, end - start + 1);
}

}  // namespace

struct Transcriber::Impl {
    whisper_context* ctx = nullptr;

    ~Impl() {
        if (ctx) whisper_free(ctx);
    }
};

Transcriber::Transcriber() : impl_(new Impl()) {}

Transcriber::~Transcriber() { delete impl_; }

bool Transcriber::loadModel(const std::string& modelPath) {
    // whisper/ggml are chatty (model dims, backend probing, per-run timings).
    // Useful when something misbehaves, too noisy for the console — send it
    // to the log file only.
    whisper_log_set([](ggml_log_level, const char* text, void*) { Log::fileOnly("%s", text); },
                    nullptr);

    whisper_context_params cparams = whisper_context_default_params();
    impl_->ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!impl_->ctx) {
        Log::error("[transcriber] failed to load model: %s\n", modelPath.c_str());
        return false;
    }
    return true;
}

std::string Transcriber::transcribe(const std::vector<float>& audio) {
    if (!impl_->ctx || audio.empty()) return {};

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = "en";
    wparams.translate = false;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.single_segment = false;
    wparams.no_context = true;
    // Keep the temperature ladder, bounded. It is what rejects a decode that
    // scored badly, and with it off (temperature_inc = 0) a repetition-looped
    // decode is detected but has no retry to be replaced by, so the loop gets
    // typed: "What about this? What about this?" for a single spoken phrase.
    // The ladder did once cost 35s on a 3.1s recording ([LAPTOP] 2026-08-31),
    // but that recording was silence — every temperature fails the no-speech
    // check, so it paid all six decodes for [BLANK_AUDIO]. main.cpp no longer
    // sends silence here. 0.4 caps the rest at three decodes (0.0/0.4/0.8).
    wparams.temperature_inc = 0.4f;
    // whisper runs on the CPU here, and on a laptop it's the slow part of the
    // whole pipeline — use every core there is.
    const unsigned hw = std::thread::hardware_concurrency();
    wparams.n_threads = static_cast<int>(hw ? hw : 4);

    // The encoder otherwise runs the model's full mel window (30s) no matter
    // how little was said, so a 4s utterance pays a 30s encoder pass. Shrink
    // the context to fit the actual audio, plus a second of headroom so the
    // tail isn't clipped. Dictation is short, so this is most of the runtime.
    const int fullCtx = whisper_model_n_audio_ctx(impl_->ctx);
    const int ctxPerSecond = fullCtx / WHISPER_CHUNK_SIZE;
    const double seconds = audio.size() / static_cast<double>(WHISPER_SAMPLE_RATE);
    const int neededCtx = static_cast<int>(std::ceil(seconds * ctxPerSecond)) + ctxPerSecond;
    wparams.audio_ctx = std::min(fullCtx, std::max(kMinAudioCtx, neededCtx));

    const auto t0 = std::chrono::steady_clock::now();
    const int rc =
        whisper_full(impl_->ctx, wparams, audio.data(), static_cast<int>(audio.size()));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (rc != 0) {
        Log::error("[transcriber] whisper_full failed\n");
        return {};
    }
    Log::info("[%.1fs audio in %.1fs (%.1fx spoken length), audio_ctx %d/%d, %d threads]\n",
              seconds, elapsed, elapsed > 0 ? seconds / elapsed : 0.0, wparams.audio_ctx,
              fullCtx, wparams.n_threads);

    std::string result;
    const int nSegments = whisper_full_n_segments(impl_->ctx);
    for (int i = 0; i < nSegments; ++i) {
        const std::string segment = trim(whisper_full_get_segment_text(impl_->ctx, i));
        // Segment timestamps go to the log file only; they are what tells a
        // doubled decode apart from someone actually saying it twice, and
        // there is no dedupe here — a repeat that reaches this point is a
        // decode worth investigating, not one to paper over.
        Log::fileOnly("[transcriber] segment %d [%lld..%lld] %s\n", i,
                      static_cast<long long>(whisper_full_get_segment_t0(impl_->ctx, i)),
                      static_cast<long long>(whisper_full_get_segment_t1(impl_->ctx, i)),
                      segment.c_str());
        if (segment.empty()) continue;
        if (!result.empty()) result += ' ';
        result += segment;
    }

    return result;
}
