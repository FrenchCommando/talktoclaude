#include <cstdio>
#include <thread>

#include "audio_capture.h"
#include "logging.h"
#include "text_injector.h"
#include "transcriber.h"
#include "trigger.h"

int main(int argc, char** argv) {
    const std::string modelPath = (argc > 1) ? argv[1] : "models/ggml-base.en.bin";

    Log::init();
    if (!Log::path().empty()) Log::info("Logging to %s\n", Log::path().c_str());

    Transcriber transcriber;
    if (!transcriber.loadModel(modelPath)) {
        Log::error("Pass the model path as the first argument if it's not at %s\n",
                modelPath.c_str());
        return 1;
    }

    AudioCapture capture;
    if (!capture.init()) {
        Log::error("Failed to initialize audio capture.\n");
        return 1;
    }

    Log::info("talktoclaude ready. Press Play/Pause (earbuds button or media key) to talk.\n");

    Trigger trigger([&](bool starting) {
        if (starting) {
            Log::info("[listening...]\n");
            capture.start();
        } else {
            Log::info("[transcribing...]\n");
            std::vector<float> audio = capture.stop();
            std::string text = transcriber.transcribe(audio);
            if (text.empty()) {
                Log::info("[no speech detected]\n");
                return;
            }
            Log::info("> %s\n", text.c_str());
            TextInjector::typeText(text);
        }
    });

    // run() pumps a Win32 message loop and blocks; that's fine, it's our
    // whole program's job right now.
    trigger.run();

    Log::close();
    return 0;
}
