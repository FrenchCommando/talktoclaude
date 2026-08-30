# talktoclaude

Voice dictation into whatever window has focus (Claude Code's terminal, in
practice), triggered by the play/pause button on Bluetooth earbuds. Local
STT via whisper.cpp — nothing leaves the machine.

See `CLAUDE.md` for the design rationale.

## Build (Windows)

Requires: MSVC (Visual Studio Build Tools, "Desktop development with C++"
workload) and CMake — both are needed for whisper.cpp and the WASAPI/Win32
code here.

whisper.cpp isn't vendored/submoduled — CMake fetches it (pinned to a
specific commit) the first time you configure:

```
cmake -B build -S .
cmake --build build --config Release
```

## Get a model

Not vendored (they're large). `setup.bat` fetches `base.en` (~142 MB) with
`curl.exe` if `models/` doesn't have it, or download it yourself from
https://huggingface.co/ggerganov/whisper.cpp/tree/main and drop it in
`models/`.

## Run

```
bin\talktoclaude.exe models\ggml-base.en.bin
```

Or just `run.bat`. `setup.bat` stages the exe and its DLLs into `bin\` and
then deletes `build\`, so `bin\` is where the runnable app lives.

## Speed

This is slow on a laptop, and that's expected. whisper runs on the CPU —
this build has no CUDA/GPU backend, and the dev machine is an i7-8550U (4
cores / 8 threads, AVX2 but no AVX-512), so `base.en` transcribes at only a
small multiple of realtime. Loading the model at startup takes ~1.5s, and
each utterance is transcribed *after* you press stop, not while you talk.

Because transcription is synchronous and blocks the trigger's message loop,
a button press during transcription is ignored until it finishes.

Every transcription logs its actual numbers, so measure rather than guess:

```
[3.4s audio in 2.1s, 1.6x realtime, 8 threads]
```

If it's too slow to use, in rough order of payoff: switch to a quantized
model (`ggml-base.en-q5_1.bin` — same downloader, noticeably faster, small
accuracy cost), drop to `tiny.en`, or build ggml with a GPU backend.

Each run writes `logs/talktoclaude-<timestamp>.log` (gitignored) with
everything the console shows plus whisper's own diagnostics; `setup.bat`
leaves its build output in `logs/setup-<timestamp>.log` the same way.

Pair your earbuds over Bluetooth, focus the window you want text typed into,
press play/pause to start talking, press again to stop — the transcript
gets typed in. A keyboard's media Play/Pause key works too, for testing
without earbuds.
