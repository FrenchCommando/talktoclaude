# talktoclaude

Voice dictation tool for Windows: speak a command, it gets typed into whatever
window has focus (Claude Code's terminal, in practice). Local, no cloud STT.

## Plan

- **Language**: C++. Single self-contained .exe, no Python/interpreter layer.
- **STT**: `whisper.cpp`, linked in directly (not shelled out to) — model size
  TBD, start with `base.en` or `small` for the CPU/accuracy tradeoff.
- **Audio capture**: WASAPI (native Win32/COM), captures mic while armed.
- **Trigger**: NOT a keyboard hotkey. Google Pixel Buds are paired to the PC
  over Bluetooth AVRCP, so the play/pause button press arrives as a system
  media key event (`VK_MEDIA_PLAY_PAUSE`). Hook that globally (low-level
  keyboard hook) as start/stop recording.
  - Note: the Buds' Assistant long-press is Android/Google-Assistant-only and
    does NOT cross over to Windows — only the standard transport buttons
    (play/pause/skip) do, via Bluetooth AVRCP. Use play/pause.
- **Text injection**: `SendInput` (Win32) to type the transcript into
  whatever window currently has focus.

## Pipeline

1. Buds play/pause press → toggle recording (WASAPI capture starts).
2. Press again (or on silence, TBD) → stop capture, run through whisper.cpp.
3. Type the resulting transcript into the focused window via `SendInput`.

## Why these choices (context from planning discussion)

- Rejected Python (`faster-whisper` + `pynput`/`pyautogui`) in favor of pure
  C++: avoids interpreter/subprocess glue, single .exe, matches "lightweight
  background utility" goal. User is fine with C++.
- Rejected wake-word / always-on VAD as the primary trigger — more moving
  parts (needs a wake-word model like openWakeWord/Porcupine) than reusing a
  physical button that's already in hand.
- Rejected NVIDIA Parakeet/Nemotron (used by macOS app FluidVoice, the
  original inspiration) — thin Windows tooling, NeMo/CoreML-first ecosystem.
  Whisper has mature, well-supported Windows C++ builds.
- FluidVoice (github.com/altic-dev/FluidVoice) itself is not portable — pure
  Swift/Xcode/macOS project, no code reuse possible, only served as the
  inspiration for the feature set.

## Setup status

- [x] CMake already installed (`C:\Program Files\CMake\bin\cmake.exe`)
- [ ] C++ compiler — installing now (Visual Studio "Build Tools for Visual
      Studio 2022", "Desktop development with C++" workload).
      Download page: https://visualstudio.microsoft.com/downloads/
- [x] whisper.cpp fetched via CMake `FetchContent` (pinned commit, not a
      submodule — avoids the "forgot to init submodules" class of pain)
- [x] Project scaffolding written (see Code layout below) — NOT YET BUILT,
      waiting on the compiler. Expect first-build issues to shake out.

## Code layout

- `CMakeLists.txt` — fetches whisper.cpp via `FetchContent` (pinned commit),
  builds `talktoclaude.exe` linking whisper + ole32/user32/winmm.
- `src/audio_capture.{h,cpp}` — WASAPI mic capture (shared mode, event-driven),
  downmixes to mono and linearly resamples to 16kHz float32 for whisper.
  Assumes the shared-mode mix format is IEEE float (normal on modern Windows;
  revisit if `GetMixFormat` ever returns PCM int).
- `src/trigger.{h,cpp}` — low-level keyboard hook (`WH_KEYBOARD_LL`) watching
  for `VK_MEDIA_PLAY_PAUSE`. A paired Bluetooth device's play/pause button
  surfaces as this same virtual key via AVRCP, so this covers both the
  earbuds and a keyboard media key (handy for testing without the buds).
  Toggle semantics: press to start, press again to stop.
- `src/transcriber.{h,cpp}` — thin whisper.cpp wrapper, loads a GGML model
  once, `transcribe(audio)` runs `whisper_full` and returns trimmed text.
- `src/text_injector.{h,cpp}` — `SendInput` with `KEYEVENTF_UNICODE`, types
  the transcript into whatever window has focus.
- `src/main.cpp` — wires it together: model path from argv[1] (default
  `models/ggml-base.en.bin`), trigger callback starts/stops capture and
  transcribes+types on stop.

## Next step

1. Finish installing the C++ Build Tools.
2. `cmake -B build -S .` then `cmake --build build --config Release` — fix
   whatever breaks (first build against a freshly vendored whisper.cpp,
   untested).
3. Fetch a GGML model (see README) and do an end-to-end test: pair earbuds,
   press play/pause, talk, confirm text lands in a focused text box.
4. Known rough edges to revisit once it's working: no VAD/auto-stop (must
   press the button twice per utterance), no silence trimming, single fixed
   language ("en") hardcoded in transcriber.cpp.
