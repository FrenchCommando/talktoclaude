#include "transcriber.h"

#include <cstdio>

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
    whisper_context_params cparams = whisper_context_default_params();
    impl_->ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!impl_->ctx) {
        fprintf(stderr, "[transcriber] failed to load model: %s\n", modelPath.c_str());
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
    // Dictation is short, single-utterance audio — keep it simple, single-threaded
    // is plenty fast for a few seconds of speech.
    wparams.n_threads = 4;

    if (whisper_full(impl_->ctx, wparams, audio.data(), static_cast<int>(audio.size())) != 0) {
        fprintf(stderr, "[transcriber] whisper_full failed\n");
        return {};
    }

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
