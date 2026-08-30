#pragma once

#include <cstdint>
#include <vector>

// Minimal WASAPI mic capture: records 16 kHz mono float32 PCM — the format
// whisper.cpp wants — while armed, into an in-memory buffer.
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Opens the default capture device. Returns false on failure (logs to stderr).
    bool init();

    // Starts recording into an internal buffer. Safe to call once per utterance.
    void start();

    // Stops recording and returns the captured audio (16kHz mono float32).
    // Clears the internal buffer so the object is ready for the next start().
    std::vector<float> stop();

    bool isRecording() const { return recording_; }

private:
    struct Impl;
    Impl* impl_;
    bool recording_ = false;
};
