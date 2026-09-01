# talktoclaude

Voice dictation for Windows: press a media button, speak, press again — the
transcript is typed into whatever window has focus, followed by Enter. Local,
no cloud STT. C++, single .exe, whisper.cpp linked in.

## Status on `[DESKTOP]` (2026-08-30)

Measurements are hardware-specific and tagged. `[DESKTOP]` = Ryzen 9 9950X3D,
16C/32T, 61.6 GB, RX 9070 XT, Realtek RTL8922 Bluetooth. `[LAPTOP]` =
i7-8550U, 4C/8T — back in service 2026-08-31 after a stint as "retired":
base.en runs ~1.0x spoken length there (vs 5.4x on `[DESKTOP]`), capture came
from the built-in Intel SST mic array at 48 kHz stereo, and its Intel
Bluetooth stack delivers presses during SCO where `[DESKTOP]`'s Realtek
doesn't.

**Working end to end, verified 2026-08-30 21:27-21:30:** seven consecutive
press → speak → auto-stop → transcribe → type cycles from the Pixel Buds
button, then a live dictation into a Claude Code session that submitted
itself. Transcription 0.2-0.5s per utterance (`audio_ctx 256`, 32 threads,
`base.en`, CPU); mic peaks 0.11-0.24 against the 0.01 silence threshold.

Two findings shaped the design that finally worked:
1. **The old SMTC claim never registered a session on this machine.** The
   silent-WAV `MediaPlayer` hack left `GetCurrentSession()` null, and AVRCP
   presses (unlike keyboard media keys, which reach `ButtonPressed`
   regardless and therefore prove nothing) are only delivered to a
   registered session. Replaced with the canonical
   `ISystemMediaTransportControlsInterop::GetForWindow` registration — the
   owner readout reports `talktoclaude.exe` once that landed.
2. **No headset press is delivered while the mic stream (SCO) is open on
   this adapter** — across every claim mechanism, with SMTC, HID
   consumer/telephony raw-input, media-key, and CallControl probes all
   silent. On `[LAPTOP]`'s Intel stack presses did arrive during SCO, so
   this is Realtek-stack behavior, not protocol. Consequence: a second
   "stop" press can never work here, which is why recording ends itself
   (see Design) — and the same auto-stop closes SCO so the *next* starting
   press finds the buds back in A2DP, which is what makes the cycle repeat.

**LE Audio must stay off.** Settings > Bluetooth & devices > Device settings >
"Use LE Audio when available" — global, not per-device, needs a restart. With
it on, the Pixel Buds contend between LE Audio and classic BR/EDR and break
three different ways (classic dies ~18s after connect; LE connects with no
audio endpoint at all; classic returns and flaps every 10-60s). With it off
they hold `classic=[110B,110C,110E,111E,1124]` and all endpoints steady.
`apx=0` (no `APXENUM` nodes) is how you confirm it's really off.

**Flow (since 2026-08-30): one press, then silence ends the utterance.**
Press once (headset in A2DP — the only state a press arrives in), speak, and
recording stops itself after ~1.5s of trailing silence or a 30s cap
(`kSpeechThreshold`/`kTrailingSilenceMs` in audio_capture.cpp are judgment
values; speech peaked ~0.03 on the buds' HFP mic). The mic stream opens on
the press and closes at auto-stop, so the buds return to A2DP between
utterances. A second press still stops early where hardware delivers one.
Two abandoned designs are recorded in git: always-open mic (constant device
state, but SCO up means no press ever arrives to start) and claim-output
pinning (solved a problem the interop registration made moot).

## Design

- **STT**: whisper.cpp, `base.en`, linked directly.
- **Capture**: WASAPI shared mode, event-driven, downmixed to 16kHz mono
  float32. Bluetooth HFP reports `16000 Hz, 1 ch` and bypasses the resampler;
  `[LAPTOP]`'s built-in mic array reports `48000 Hz, 2 ch`, so the 48k→16k
  resample + stereo downmix path is live there and transcribed correctly
  (2026-08-31) — no longer dead code.
- **Trigger**: SMTC. An AVRCP press does *not* surface as a `WH_KEYBOARD_LL`
  event and is only delivered to a registered media session, so we register
  one the canonical desktop way: `ISystemMediaTransportControlsInterop::
  GetForWindow` on a hidden window, display metadata, `PlaybackStatus =
  Playing`, and listen for `ButtonPressed`. Status is re-asserted every 3s
  (anything that starts playing takes the button); Ctrl+Alt+V forces it.
  Synthesized media *keys* reach `ButtonPressed` even without a session —
  never use them as proof the AVRCP path works.
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
- `run-small.bat` / `run-turbo.bat` — run.bat with `small.en` /
  `large-v3-turbo`, fetching the model on first use (setup.bat only ships
  base.en; the fetch goes to `.part` then renames, so a truncated download
  never looks like a model).
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

- `[DESKTOP]` Model comparison, measured 2026-08-30 (CPU, 32 threads, HFP
  mic):
  | model | speed | notes |
  |---|---|---|
  | base.en | 0.4s / 2.1s audio (5.4x) | mishears short utterances ("then commit and close" → "the milk and clothes") |
  | small.en | 1.2-1.5s / ~4.5s (3.2-3.6x) | latency fine; accuracy not obviously better |
  | large-v3-turbo | 1.5x realtime degrading to **0.3x** (18s for 5.6s) across four utterances in one run | hallucination loops on top; unusable on CPU |

  **All accuracy readings above are confounded: they were taken in a loud
  environment** (through the HFP mic, which degrades under noise first).
  Speed numbers stand; the accuracy column is not a model verdict —
  re-judge in a quiet room before drawing model conclusions. turbo is the
  model to revisit *after* the GPU backend exists, not before.
- GPU unused: `no GPU found`, `backends = 1`. whisper.cpp is built without a
  backend, so `use gpu = 1` is inert and the RX 9070 XT idles. Vulkan supports
  AMD — probably the largest available speedup, and the precondition for
  running anything bigger than small.en (see the model table).
- whisper's non-speech markers (`[BLANK_AUDIO]`, `[ Silence ]`) get typed and
  submitted like any other transcript. Deliberate: a `[ Silence ]` landing in
  the window is useful feedback that the press-and-capture path ran.
- `base.en` accuracy on short HFP utterances is mediocre: live example,
  "then commit and close" → "the milk and clothes" (2.6s utterance). Fuel
  for the GPU/`small.en` re-evaluation above.
- No VAD/auto-stop, no silence trimming, language hardcoded to "en".
- Transcription is synchronous and blocks the trigger's message loop, but a
  press during it is *queued*, not lost: SMTC `ButtonPressed` fires on a
  WinRT threadpool thread and is posted into the loop. It used to be handled
  directly on that threadpool thread — which meant a press mid-transcription
  ran a second `whisper_full` concurrently on the same context, and on
  `[LAPTOP]` (2026-08-31) that hit ggml's `!isnan(sumf)` assert and killed
  the process. The same session showed one 3.1s utterance stuck 35s+ inside
  `whisper_full` (why the press-during-transcription window was open at
  all); `temperature_inc = 0` now keeps whisper from retry-laddering like
  that.
- `n_threads` is `hardware_concurrency()` (32 here). Never A/B'd against a
  smaller value, but the 0.4s measurement suggests it isn't hurting.
