# talktoclaude

Voice dictation for Windows: press a media button, speak, press again — the
transcript is typed into whatever window has focus, followed by Enter. Local,
no cloud STT. C++, single .exe, whisper.cpp linked in.

## Status on `[DESKTOP]` (2026-08-30)

Measurements are hardware-specific and tagged. `[DESKTOP]` = Ryzen 9 9950X3D,
16C/32T, 61.6 GB, RX 9070 XT, Realtek RTL8922 Bluetooth. `[LAPTOP]` = retired
i7-8550U; treat its numbers as history, not guidance.

| path | state |
|---|---|
| synthesized `VK_MEDIA_PLAY_PAUSE` | full cycle works: both presses, capture, transcribe, type |
| OnePlus Bullets Wireless Z button | one press seen once; never a full cycle |
| Pixel Buds Pro 2 button | **zero presses ever**, any configuration |

So the pipeline downstream of the button is sound; physical headset buttons
are the unsolved part. Speed: 2.1s audio in 0.4s (`audio_ctx 256`, 32 threads,
`base.en`, CPU). One sample.

**LE Audio must stay off.** Settings > Bluetooth & devices > Device settings >
"Use LE Audio when available" — global, not per-device, needs a restart. With
it on, the Pixel Buds contend between LE Audio and classic BR/EDR and break
three different ways (classic dies ~18s after connect; LE connects with no
audio endpoint at all; classic returns and flaps every 10-60s). With it off
they hold `classic=[110B,110C,110E,111E,1124]` and all endpoints steady.
`apx=0` (no `APXENUM` nodes) is how you confirm it's really off.

**The buds' button reaches Windows but never us** — it pauses a YouTube video,
so it travels the SMTC layer fine. Reported session ownership is not the
variable: the OnePlus press and the buds' silence both happened under
`(no session at all)`. Untested hypothesis: the silent claim stream renders to
the *default* output device, so when that's a Bluetooth headset its endpoint
drops out from under the claim (`src/trigger.cpp:179` predicts this). Fix
would be pinning `MediaPlayer.AudioDevice`; zero-code test is to set the
default output to a wired device and press.

## Design

- **STT**: whisper.cpp, `base.en`, linked directly.
- **Capture**: WASAPI shared mode, event-driven, downmixed to 16kHz mono
  float32. With LE Audio off every machine reports `16000 Hz, 1 ch` (HFP), so
  the 48k→16k resample path is effectively dead code and stays untested.
- **Trigger**: SMTC. An AVRCP press does *not* surface as a `WH_KEYBOARD_LL`
  event, so we make this process the "now playing" session — a silent looping
  WAV via `MediaPlayer` — and listen for `ButtonPressed`. `MediaPlayer` is
  recreated every 3s to re-claim, since pause/play on the existing one
  coalesces into no state transition. Ctrl+Alt+V forces a re-claim.
- **Injection**: `SendInput` + `KEYEVENTF_UNICODE`, then `VK_RETURN`.

**Injection has no target window.** Whatever has focus when transcription
*finishes* gets the keystrokes and the Enter. This has already fired a stray
prompt into an unrelated Claude session. Never drive the trigger
programmatically without controlling focus first.

## Rejected alternatives (don't re-propose)

- Python (`faster-whisper` + `pynput`) — interpreter/subprocess glue against a
  single-.exe goal.
- Wake-word / always-on VAD as the *primary* trigger — needs a model
  (openWakeWord/Porcupine) to replace a button already in hand.
- NVIDIA Parakeet/Nemotron — NeMo/CoreML-first, thin Windows tooling.
- FluidVoice (the macOS app this imitates) — pure Swift, nothing reusable.
- `WH_KEYBOARD_LL` hook — an AVRCP press never reaches it; hence SMTC.

## Running

- `setup.bat` — finds VS Build Tools via `vswhere`, builds with CMake/NMake,
  fetches the model, stages exe + whisper/ggml DLLs into `bin/`, deletes
  `build/`. Re-running rebuilds whisper.cpp from scratch. Batch files here
  need CRLF — cmd.exe mis-parses LF-only.
- `run.bat [model path]` — defaults to `models\ggml-base.en.bin`.
- Close the console or Ctrl+C to quit.
- `logs/` (gitignored): `talktoclaude-<stamp>.log` per run, plus setup logs.

## Code layout

- `CMakeLists.txt` — FetchContent whisper.cpp (pinned), links
  ole32/user32/winmm/windowsapp, copies DLLs next to the exe.
- `src/audio_capture.{h,cpp}` — WASAPI capture. Assumes IEEE float mix format.
  `CoInitializeEx(COINIT_MULTITHREADED)` on the main thread; `trigger.cpp`'s
  WinRT init **must** match this apartment or it throws `RPC_E_CHANGED_MODE`.
- `src/trigger.{h,cpp}` — SMTC trigger. Accepts both `Play` and `Pause` (no
  combined enum member). Logs the button value *before* filtering, so a
  delivered-but-unhandled press is still visible.
- `src/transcriber.{h,cpp}` — whisper wrapper. Shrinks `wparams.audio_ctx` to
  fit the audio, which is the 5.5x win over the fixed 30s mel window; floor is
  `kMinAudioCtx = 256`, a judgement value whose accuracy was never evaluated.
- `src/text_injector.{h,cpp}` — see the injection warning above.
- `src/logging.{h,cpp}` — console + file; `Log::fileOnly` for whisper's chatter.
- `src/main.cpp` — wires it together.

## Debugging Bluetooth

- **Connect a different headset first.** One working OnePlus retired every
  adapter/driver/coexistence theory in a single step.
- **Use `Get-PnpDevice`'s `.Present`.** DEVPKEY `{83da6326-...},15` is *not*
  presence — it reported 0 live nodes for a device `.Present` showed with 24.
- `logs/btwatch.ps1 -Addr <addr>` logs link transitions: `dev`, `classic`
  (AVRCP lives only here), `apx` (LE Audio), `ep`. Sample inline rather than
  running two pollers — they race on the same log and lose transitions.
- The Windows event log shows **nothing** during full disconnects.
- Present profile nodes ≠ audio flowing. Confirm sound before calling it fixed.
- Chrome holds the SMTC session until the process exits — closing the tab is
  not enough. The owner readout has never once reported `talktoclaude`, so
  treat `(no session at all)` as "unknown", not "we don't have it".

## Known rough edges

- GPU unused: `no GPU found`, `backends = 1`. whisper.cpp is built without a
  backend, so `use gpu = 1` is inert and the RX 9070 XT idles. Vulkan supports
  AMD — probably the largest available speedup.
- whisper's non-speech markers (`[BLANK_AUDIO]`, `[ Pause ]`) get typed and
  submitted like any other transcript.
- No VAD/auto-stop, no silence trimming, language hardcoded to "en".
- Transcription is synchronous and blocks the SMTC loop, so a press during it
  is lost.
- `n_threads` is `hardware_concurrency()` (32 here). Never A/B'd against a
  smaller value, but the 0.4s measurement suggests it isn't hurting.
