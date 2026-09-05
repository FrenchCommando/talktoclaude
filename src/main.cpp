#include <string>

#include <cstdio>
#include <cstring>

#include "audio_capture.h"
#include "logging.h"
#include "paths.h"
#include "text_injector.h"
#include "transcriber.h"
#include "trigger.h"

int main(int argc, char** argv) {
    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        printf("usage: talktoclaude [model.bin]\n\n"
               "Press the Play/Pause button on a Bluetooth headset (or a keyboard media key),\n"
               "speak, and stop; the transcript is typed into the focused window, then Enter.\n"
               "Without an argument the base.en whisper model is used, downloaded on first run\n"
               "into %%LOCALAPPDATA%%\\talktoclaude\\models. Logs go to the logs\\ folder next\n"
               "to it (or the repo's logs\\ when run from a checkout). Close the window to quit.\n");
        return 0;
    }

    Log::init(Paths::logDir());
    if (!Log::path().empty()) Log::info("Logging to %s\n", Log::path().c_str());

    const std::string modelPath = (argc > 1) ? argv[1] : Paths::defaultModel();
    if (modelPath.empty()) {
        Log::error("No model. Pass a path to a ggml whisper model as the first argument.\n");
        Log::close();
        return 1;
    }

    Transcriber transcriber;
    if (!transcriber.loadModel(modelPath)) {
        Log::error("Pass the model path as the first argument if it's not at %s\n",
                   modelPath.c_str());
        Log::close();
        return 1;
    }

    AudioCapture capture;
    if (!capture.init()) {
        Log::error("Failed to initialize audio capture.\n");
        Log::close();
        return 1;
    }

    Log::info("talktoclaude ready. Press Play/Pause to talk; recording ends itself "
              "after ~1.5s of silence (or a second press, where the hardware delivers one).\n");

    // The window the user was in when they pressed the button. The transcript
    // goes there or nowhere; see TextInjector::typeText.
    TextInjector::Target target = nullptr;

    const auto startListening = [&] {
        target = TextInjector::foregroundTarget();
        Log::info("[listening...]\n");
        capture.start();
    };

    const auto finishUtterance = [&] {
        std::vector<float> audio = capture.stop();
        if (audio.empty() || capture.lastPeak() == 0.0f) {
            Log::info("[no audio captured - the mic delivered silence]\n");
            return;
        }
        // A recording that never crossed the speech threshold is the
        // decoder's worst input: every temperature fails the no-speech
        // check, so it walks the whole fallback ladder to arrive at
        // [BLANK_AUDIO]. Don't decode it. Reachable only via the paths
        // that skip the capture's own sawSpeech test — the 30s cap and a
        // second button press; a trailing-silence auto-stop already
        // implies the same threshold was crossed.
        if (capture.lastPeak() < AudioCapture::speechThreshold()) {
            Log::info("[nothing said - peak %.4f below the %.4f speech threshold]\n",
                      capture.lastPeak(), AudioCapture::speechThreshold());
            return;
        }
        Log::info("[transcribing...]\n");
        const std::string text = transcriber.transcribe(audio);
        if (text.empty()) {
            Log::info("[no speech detected]\n");
            return;
        }
        Log::info("> %s\n", text.c_str());
        TextInjector::typeText(text, target);
    };

    // The capture's own state decides what a press means, so a press and
    // an auto-stop can never disagree about whether recording is on.
    Trigger trigger(
        [&] {
            if (capture.isRecording()) finishUtterance();
            else startListening();
        },
        [&] {
            if (capture.isRecording()) finishUtterance();
        });

    // The utterance ends itself: trailing silence (or the hard cap) posts a
    // stop into the trigger's loop. No second button press is needed — and
    // on `[DESKTOP]`'s adapter none would arrive while the mic is open.
    capture.onUtteranceEnd([&trigger] { trigger.requestStop(); });

    // run() pumps a Win32 message loop and blocks; that's fine, it's our
    // whole program's job right now.
    trigger.run();

    Log::close();
    return 0;
}
