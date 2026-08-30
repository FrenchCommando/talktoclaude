#include "transcriber.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "logging.h"

#include "whisper.h"

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
    // whisper runs on the CPU here, and on a laptop it's the slow part of the
    // whole pipeline — use every core there is.
    unsigned hw = std::thread::hardware_concurrency();
    wparams.n_threads = static_cast<int>(hw ? hw : 4);

    auto t0 = std::chrono::steady_clock::now();
    int rc = whisper_full(impl_->ctx, wparams, audio.data(), static_cast<int>(audio.size()));
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (rc != 0) {
        Log::error("[transcriber] whisper_full failed\n");
        return {};
    }
    double seconds = audio.size() / 16000.0;
    Log::info("[%.1fs audio in %.1fs, %.1fx realtime, %d threads]\n", seconds, elapsed,
              elapsed > 0 ? seconds / elapsed : 0.0, wparams.n_threads);

    std::string result;
    int nSegments = whisper_full_n_segments(impl_->ctx);
    for (int i = 0; i < nSegments; ++i) {
        result += whisper_full_get_segment_text(impl_->ctx, i);
    }

    // Trim leading/trailing whitespace whisper tends to emit.
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return {};
    return result.substr(start, end - start + 1);
}
