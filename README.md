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

Measured on the dev machine (i7-8550U, 4 cores / 8 threads, AVX2, no
AVX-512; CPU only — this build has no GPU backend):

```
[3.8s audio in 18.1s, 0.2x realtime, 8 threads]
[3.2s audio in 16.5s, 0.2x realtime, 8 threads]
[4.2s audio in 18.4s, 0.2x realtime, 8 threads]
```

Read that `0.2x` carefully — the denominator is what you *said*, but it is
not what whisper *processed*. Whisper's encoder runs over a fixed 30-second
mel window (`n_audio_ctx = 1500`), zero-padded, so a 4-second utterance costs
the same encoder pass as a 30-second one. Against the real 30s window those
runs are ~1.8x realtime, which is ordinary for `base.en` on this CPU. Nothing
is malfunctioning; a short sentence is just paying full price.

So expect ~15-20 seconds between pressing stop and the text appearing, mostly
independent of how long you spoke. Model load at startup is a further ~1.5s,
once. Transcription is synchronous and blocks the trigger's message loop, so
a button press during it isn't seen until it finishes.

Ways out, in order of payoff:

- **`wparams.audio_ctx`** — shrink the encoder context to match the actual
  audio instead of always encoding 30s (whisper.cpp's `--audio-ctx`). Close
  to a linear encoder speedup for short utterances. Not implemented yet.
- **Thread count** — `n_threads` is `hardware_concurrency()` (8 here), but
  there are only 4 physical cores; ggml often does no better, or worse, on
  hyperthreads. Worth an A/B.
- **Smaller/quantized model** — `ggml-base.en-q5_1.bin` or `tiny.en`.
- **A GPU backend** — build ggml with one.

Each run writes `logs/talktoclaude-<timestamp>.log` (gitignored) with
everything the console shows plus whisper's own diagnostics; `setup.bat`
leaves its build output in `logs/setup-<timestamp>.log` the same way.

Pair your earbuds over Bluetooth, focus the window you want text typed into,
press play/pause to start talking, press again to stop — the transcript
gets typed in. A keyboard's media Play/Pause key works too, for testing
without earbuds.
