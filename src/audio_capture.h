#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// Minimal WASAPI mic capture: records 16 kHz mono float32 PCM — the format
// whisper.cpp wants — while armed, into an in-memory buffer.
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Opens the default capture device. Returns false on failure (logs to
    // stderr). The WASAPI stream is started per-utterance (see start), NOT
    // here: with a Bluetooth headset an open capture stream means SCO/
    // hands-free mode, and on `[DESKTOP]`'s Realtek adapter no button press
    // is delivered while SCO is up. The headset must sit in A2DP between
    // utterances so the *starting* press can arrive; the utterance is then
    // ended by silence detection (onUtteranceEnd), never by a second press.
    bool init();

    // Starts the stream and begins recording. Safe to call once per
    // utterance. Note the first ~0.5s may be silence while the Bluetooth
    // SCO link ramps up.
    void start();

    // Stops recording and the stream, and returns the captured audio
    // (16kHz mono float32). Clears the internal buffer so the object is
    // ready for the next start().
    std::vector<float> stop();

    // Called (from the capture thread) when the utterance is over: trailing
    // silence after speech, or the hard time cap. The callee should arrange
    // for stop() to be called on its own thread — not call it re-entrantly.
    void onUtteranceEnd(std::function<void()> callback);

    bool isRecording() const { return recording_; }

    // Loudest sample of the last stop()'d capture. Exactly 0.0 means the
    // device handed over nothing but digital silence — a muted mic, or a
    // Bluetooth headset whose mic endpoint isn't live — which is worth
    // reporting differently from "whisper heard no words".
    float lastPeak() const { return lastPeak_; }

private:
    struct Impl;
    Impl* impl_;
    bool recording_ = false;
    float lastPeak_ = 0.0f;
};
