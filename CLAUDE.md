# talktoclaude

Voice dictation tool for Windows: speak a command, it gets typed into whatever
window has focus (Claude Code's terminal, in practice). Local, no cloud STT.

Working end-to-end: earbuds button starts/stops recording, whisper.cpp
transcribes, the text is typed into the focused window followed by Enter so
it submits on its own.

## Design

- **Language**: C++. Single self-contained .exe, no Python/interpreter layer.
- **STT**: `whisper.cpp`, linked in directly (not shelled out to), `base.en`
  model.
- **Audio capture**: WASAPI (native Win32/COM), captures mic while armed.
- **Trigger**: Google Pixel Buds paired over Bluetooth AVRCP. AVRCP
  play/pause does **not** surface as a `WH_KEYBOARD_LL` keyboard event on
  this system — confirmed by testing: a real keyboard's media key does show
  up as `VK_MEDIA_PLAY_PAUSE` there, the earbuds' AVRCP press does not, and
  pressing it just paused whatever video was playing instead of reaching our
  hook. It's routed through Windows' System Media Transport Controls (SMTC)
  layer straight to whichever app owns the "now playing" session.
  Interception is instead done by making `talktoclaude` itself the active
  SMTC session (silent looping audio via `MediaPlayer`) and listening for
  `SystemMediaTransportControls.ButtonPressed`. See `src/trigger.cpp`.
  - Note: the Buds' Assistant long-press is Android/Google-Assistant-only and
    does NOT cross over to Windows — only the standard transport buttons
    (play/pause/skip) do, via AVRCP. Use play/pause.
- **Text injection**: `SendInput` (Win32) to type the transcript into
  whatever window currently has focus, followed by a synthesized Enter so
  it submits without a manual keypress.

## Pipeline

1. Buds play/pause press → SMTC `ButtonPressed` → toggle recording (WASAPI
   capture starts).
2. Press again → stop capture, run through whisper.cpp.
3. Type the resulting transcript into the focused window via `SendInput`,
   then send Enter.

Caveat (expected, not a bug): `SendInput` has no target window of its own —
whatever has focus *when transcription finishes* gets the keystrokes. Make
sure the intended text field is focused before pressing stop (not
necessarily before pressing start).

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
- Originally planned a `WH_KEYBOARD_LL` keyboard hook for the trigger
  (assuming AVRCP play/pause shows up as `VK_MEDIA_PLAY_PAUSE`) — abandoned
  after testing showed the earbuds' press never reaches that hook; switched
  to claiming the SMTC "now playing" session instead (see Design above).

## Setup

- `setup.bat` — locates the VS Build Tools toolchain via `vswhere` +
  `vcvarsall.bat` (no need for a special Developer Command Prompt),
  configures + builds with CMake/NMake, and fetches `ggml-base.en.bin` if
  `models/` doesn't have it yet. Batch files in this repo need CRLF line
  endings — cmd.exe mis-parses LF-only files.
- `run.bat` — runs `build\talktoclaude.exe models\ggml-base.en.bin`.
  Double-click it or run from a terminal; no dev environment needed to run,
  only to build.
- To close the app: close the console window, or Ctrl+C in it.

## Code layout

- `CMakeLists.txt` — fetches whisper.cpp via `FetchContent` (pinned commit),
  builds `talktoclaude.exe` linking whisper + ole32/user32/winmm/windowsapp
  (WinRT projection, for SMTC). Copies whisper/ggml's shared-lib DLLs next
  to the exe post-build — they're built into `build/bin/`, and Windows only
  auto-loads DLLs from the exe's own directory or PATH.
- `src/audio_capture.{h,cpp}` — WASAPI mic capture (shared mode, event-driven),
  downmixes to mono and linearly resamples to 16kHz float32 for whisper.
  Assumes the shared-mode mix format is IEEE float (normal on modern Windows;
  revisit if `GetMixFormat` ever returns PCM int). Calls
  `CoInitializeEx(COINIT_MULTITHREADED)` on the main thread — `trigger.cpp`'s
  WinRT init has to match this apartment type (MTA) or it throws
  `RPC_E_CHANGED_MODE`.
- `src/trigger.{h,cpp}` — WinRT `SystemMediaTransportControls`-based trigger
  (see Design above). Plays a tiny silent looping WAV via `MediaPlayer` to
  claim the SMTC session, listens for `ButtonPressed` (checks for both
  `Play` and `Pause` button values — there's no combined `PlayPause` enum
  member). Toggle semantics: press to start, press again to stop.
- `src/transcriber.{h,cpp}` — thin whisper.cpp wrapper, loads a GGML model
  once, `transcribe(audio)` runs `whisper_full` and returns trimmed text.
- `src/text_injector.{h,cpp}` — `SendInput` with `KEYEVENTF_UNICODE` to type
  the transcript into whatever window has focus, then a synthesized
  `VK_RETURN` so it submits.
- `src/main.cpp` — wires it together: model path from argv[1] (default
  `models/ggml-base.en.bin`), trigger callback starts/stops capture and
  transcribes+types on stop.

## Known rough edges (not blocking, revisit if annoying)

- No VAD/auto-stop — must press the button twice per utterance.
- No silence trimming.
- Single fixed language ("en") hardcoded in transcriber.cpp.
- Transcription is synchronous and blocks the SMTC message loop, so a button
  press during transcription won't register until it finishes.
