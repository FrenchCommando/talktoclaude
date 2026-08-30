#pragma once

#include <string>
#include <vector>

// Thin wrapper around whisper.cpp: loads a GGML model once, transcribes
// 16kHz mono float32 PCM buffers on demand.
class Transcriber {
public:
    Transcriber();
    ~Transcriber();

    // modelPath: path to a GGML whisper model, e.g. models/ggml-base.en.bin
    // (see README for how to fetch one — not vendored, they're large).
    bool loadModel(const std::string& modelPath);

    // audio: 16kHz mono float32 samples, as produced by AudioCapture::stop().
    // Returns the transcribed text (trimmed), or empty string on failure /
    // no speech detected.
    std::string transcribe(const std::vector<float>& audio);

private:
    struct Impl;
    Impl* impl_;
};
