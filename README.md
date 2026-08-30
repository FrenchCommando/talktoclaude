# talktoclaude

Voice dictation into whatever window has focus (Claude Code's terminal, in
practice), triggered by the play/pause button on Bluetooth earbuds. Local
STT via whisper.cpp — nothing leaves the machine.

See `CLAUDE.md` for the design rationale.

## Build (Windows)

Requires: MSVC (Visual Studio Build Tools, "Desktop development with C++"
workload) and CMake — both are needed for whisper.cpp and the WASAPI/Win32
code here.

```
git submodule update --init --recursive   # if third_party/whisper.cpp is empty
cmake -B build -S .
cmake --build build --config Release
```

## Get a model

Not vendored (they're large). Fetch one of whisper.cpp's GGML models, e.g.:

```
third_party/whisper.cpp/models/download-ggml-model.cmd base.en
```

or download directly from
https://huggingface.co/ggerganov/whisper.cpp/tree/main and drop it in
`models/`.

## Run

```
build/Release/talktoclaude.exe path\to\ggml-base.en.bin
```

Pair your earbuds over Bluetooth, focus the window you want text typed into,
press play/pause to start talking, press again to stop — the transcript
gets typed in. A keyboard's media Play/Pause key works too, for testing
without earbuds.
