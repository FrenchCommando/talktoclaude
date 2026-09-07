#include <cstdio>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "audio_capture.h"
#include "logging.h"
#include "paths.h"
#include "text_injector.h"
#include "transcriber.h"
#include "trigger.h"

namespace {

const char* const kUsage =
    "usage: talktoclaude [--console] [model.bin]\n\n"
    "Runs in the notification area. Press the Play/Pause button on a Bluetooth\n"
    "headset (or a keyboard media key), speak, and stop; the transcript is typed\n"
    "into the focused window, then Enter. Click the icon to open the log folder\n"
    "or quit.\n\n"
    "Without a model argument the base.en whisper model is used, downloaded on\n"
    "first run into %LOCALAPPDATA%\\talktoclaude\\models. Logs go to the logs\\\n"
    "folder next to it (or the repo's logs\\ when run from a checkout).\n"
    "--console also prints everything to a console window.\n";

// The exe is a windows-subsystem app, so it has no console of its own.
// --console attaches to the terminal it was launched from, or opens one.
void attachConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    Log::setConsole(true);
}

// Something went wrong before the tray icon could say so. With a console
// the message is already there; without one, show it.
void fatal(const char* message, bool haveConsole) {
    Log::error("%s\n", message);
    if (!haveConsole) MessageBoxA(nullptr, message, "talktoclaude", MB_OK | MB_ICONERROR);
}

}  // namespace

int main(int argc, char** argv) {
    bool console = false;
    std::string modelArg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            attachConsole();
            printf("%s", kUsage);
            return 0;
        }
        if (std::strcmp(argv[i], "--console") == 0) console = true;
        else modelArg = argv[i];
    }
    if (console) attachConsole();

    // Two instances would fight over the SMTC session and both type. The
    // second one leaves quietly; the tray icon already shows the first.
    const HANDLE instance = CreateMutexW(nullptr, TRUE, L"talktoclaude-single-instance");
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        fatal("talktoclaude is already running (see the notification area).", console);
        return 2;
    }

    Log::init(Paths::logDir());
    if (!Log::path().empty()) Log::info("Logging to %s\n", Log::path().c_str());

    // Declared before the trigger so its callbacks can reference them; the
    // heavy work (model load, mic) happens after the tray icon is up.
    Transcriber transcriber;
    AudioCapture capture;

    // The window the user was in when they pressed the button. The transcript
    // goes there or nowhere; see TextInjector::typeText.
    TextInjector::Target target = nullptr;
    Trigger* triggerPtr = nullptr;

    const auto startListening = [&] {
        target = TextInjector::foregroundTarget();
        Log::info("[listening...]\n");
        triggerPtr->setState(Tray::State::Listening, "talktoclaude: listening");
        capture.start();
    };

    const auto finishUtterance = [&] {
        triggerPtr->setState(Tray::State::Transcribing, "talktoclaude: transcribing");
        std::vector<float> audio = capture.stop();
        std::string tip = "talktoclaude: ready";
        if (audio.empty() || capture.lastPeak() == 0.0f) {
            Log::info("[no audio captured - the mic delivered silence]\n");
            tip = "talktoclaude: the mic delivered silence";
        } else if (capture.lastPeak() < AudioCapture::speechThreshold()) {
            // A recording that never crossed the speech threshold is the
            // decoder's worst input: every temperature fails the no-speech
            // check, so it walks the whole fallback ladder to arrive at
            // [BLANK_AUDIO]. Don't decode it. Reachable only via the paths
            // that skip the capture's own sawSpeech test — the 30s cap and a
            // second button press; a trailing-silence auto-stop already
            // implies the same threshold was crossed.
            Log::info("[nothing said - peak %.4f below the %.4f speech threshold]\n",
                      capture.lastPeak(), AudioCapture::speechThreshold());
            tip = "talktoclaude: nothing said";
        } else {
            Log::info("[transcribing...]\n");
            const std::string text = transcriber.transcribe(audio);
            if (text.empty()) {
                Log::info("[no speech detected]\n");
                tip = "talktoclaude: no speech detected";
            } else {
                Log::info("> %s\n", text.c_str());
                TextInjector::typeText(text, target);
                tip = "talktoclaude: " + text.substr(0, 100);
            }
        }
        triggerPtr->setState(Tray::State::Idle, tip);
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
        },
        Paths::logDir());
    triggerPtr = &trigger;

    if (!trigger.start()) {
        fatal("Couldn't create the tray window.", console);
        Log::close();
        return 1;
    }

    std::string modelPath = modelArg;
    // The install never ships the model, so a download means this is the
    // first launch — the one time a "ready" balloon earns its place. A
    // window would take focus, which the injector must never see.
    bool firstRun = false;
    if (modelPath.empty()) {
        bool announced = false;
        modelPath = Paths::defaultModel([&](int percent) {
            if (!announced) {
                trigger.notify("talktoclaude", "First run: downloading the speech model (142 MB), once.");
                announced = true;
                firstRun = true;
            }
            char tip[64];
            snprintf(tip, sizeof(tip), "talktoclaude: downloading model %d%%", percent);
            trigger.setState(Tray::State::Busy, tip);
            if (console) { printf("\r[model] %d%%", percent); fflush(stdout); }
        });
        if (console && announced) printf("\n");
    }
    if (modelPath.empty()) {
        fatal("No model. Pass a path to a ggml whisper model, or check the log for the download error.",
              console);
        Log::close();
        return 1;
    }

    trigger.setState(Tray::State::Busy, "talktoclaude: loading model");
    if (!transcriber.loadModel(modelPath)) {
        fatal(("Couldn't load the model at " + modelPath).c_str(), console);
        Log::close();
        return 1;
    }

    if (!capture.init()) {
        fatal("Failed to initialize audio capture. Is a microphone connected?", console);
        Log::close();
        return 1;
    }

    // The utterance ends itself: trailing silence (or the hard cap) posts a
    // stop into the trigger's loop. No second button press is needed — and
    // on `[DESKTOP]`'s adapter none would arrive while the mic is open.
    capture.onUtteranceEnd([&trigger] { trigger.requestStop(); });

    Log::info("talktoclaude ready. Press Play/Pause to talk; recording ends itself "
              "after ~1.5s of silence (or a second press, where the hardware delivers one).\n");
    trigger.setState(Tray::State::Idle, "talktoclaude: ready");
    if (firstRun) {
        trigger.notify("talktoclaude is ready",
                       "Press Play/Pause on your headset, speak, and the words are typed "
                       "where the cursor is. It lives here in the notification area.");
    }

    // run() pumps a Win32 message loop and blocks; that's fine, it's our
    // whole program's job right now.
    trigger.run();

    Log::close();
    if (instance) CloseHandle(instance);
    return 0;
}
