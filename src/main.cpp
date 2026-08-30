#include <cstdio>
#include <thread>

#include "audio_capture.h"
#include "text_injector.h"
#include "transcriber.h"
#include "trigger.h"

int main(int argc, char** argv) {
    std::string modelPath = (argc > 1) ? argv[1] : "models/ggml-base.en.bin";

    Transcriber transcriber;
    if (!transcriber.loadModel(modelPath)) {
        fprintf(stderr, "Pass the model path as the first argument if it's not at %s\n",
                modelPath.c_str());
        return 1;
    }

    AudioCapture capture;
    if (!capture.init()) {
        fprintf(stderr, "Failed to initialize audio capture.\n");
        return 1;
    }

    printf("talktoclaude ready. Press Play/Pause (earbuds button or media key) to talk.\n");

    Trigger trigger([&](bool starting) {
        if (starting) {
            printf("[listening...]\n");
            capture.start();
        } else {
            printf("[transcribing...]\n");
            std::vector<float> audio = capture.stop();
            std::string text = transcriber.transcribe(audio);
            if (text.empty()) {
                printf("[no speech detected]\n");
                return;
            }
            printf("> %s\n", text.c_str());
            TextInjector::typeText(text);
        }
    });

    // run() pumps a Win32 message loop and blocks; that's fine, it's our
    // whole program's job right now.
    trigger.run();

    return 0;
}
