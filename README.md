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
[3.8s audio in 18.1s (0.2x spoken length), audio_ctx 1500/1500]   before
[3.7s audio in  3.3s (1.1x spoken length), audio_ctx  256/1500]   after
[2.1s audio in  3.5s (0.6x spoken length), audio_ctx  256/1500]
```

Whisper's encoder runs a fixed 30-second mel window, zero-padded, so a 4s
utterance used to cost the same encoder pass as a 30s one — that's the 18.1s.
`transcribe()` now shrinks `wparams.audio_ctx` to fit the actual audio
(derived from `whisper_model_n_audio_ctx` / `WHISPER_CHUNK_SIZE`, plus a
second of headroom), which is the 5.5x.

Anything short clamps to the `kMinAudioCtx = 256` floor, so expect a roughly
flat ~3.5s between pressing stop and the text appearing. Model load at
startup is a further ~1.5s, once. Transcription is synchronous and blocks the
trigger's message loop, so a button press during it isn't seen until it
finishes.

The floor is a judgement value from whisper.cpp's `--audio-ctx` guidance, not
something measured here. If words go missing from the end of longer
sentences, or short ones come back garbled, raise it.

`run.bat` takes an optional model path (`run.bat models\ggml-tiny.en.bin`)
so another model can be tried without a rebuild — the path is just argv[1].

Bigger isn't automatically better here: `small.en-q5_1` measured 40.9s for
2.1s of audio, ~10x `base.en`, not the ~2x its parameter count suggests. This
CPU has AVX2 but no VNNI, so q5_1's on-the-fly dequantization costs more than
it saves. Measure before switching.

Further levers, untried:

- **Thread count** — `n_threads` is `hardware_concurrency()` (8 here) on 4
  physical cores; ggml often does no better, or worse, on hyperthreads.
- **`tiny.en`** — 78 MB, roughly 3x faster, at an accuracy cost.
- **A GPU backend** — build ggml with one.

Each run writes `logs/talktoclaude-<timestamp>.log` (gitignored) with
everything the console shows plus whisper's own diagnostics; `setup.bat`
leaves its build output in `logs/setup-<timestamp>.log` the same way.

Pair your earbuds over Bluetooth, focus the window you want text typed into,
press play/pause to start talking, press again to stop — the transcript
gets typed in. A keyboard's media Play/Pause key works too, for testing
without earbuds.
