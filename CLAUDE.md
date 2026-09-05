# talktoclaude

Voice dictation for Windows: press a media button, speak, stop speaking — the
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
  float32. `stop()` trims the silence either side of the speech (keeping
  300ms/250ms) before handing the audio over — see the doubled-transcript
  note under Known rough edges. It also calls `Reset()` as well as `Stop()`:
  `Stop()` only pauses, and the capture loop drains packets on the stream
  event, so anything queued at stop time would otherwise be appended to the
  *next* utterance. Bluetooth HFP reports `16000 Hz, 1 ch` and bypasses the resampler;
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

**Injection is guarded by a focus check, not a target window.** `SendInput`
goes to whatever has focus when transcription *finishes*, and that once
fired a stray prompt into an unrelated Claude session after focus moved
mid-utterance. Since 2026-09-05 the foreground window is recorded at the
starting press and the transcript is dropped (logged with both titles) if a
different window is in front at injection time. It still cannot *aim*: never
drive the trigger programmatically without controlling focus first. The
trigger keeps no recording flag of its own either; main.cpp reads the
capture's state to decide what a press means, so a press and an auto-stop
can't disagree.

## Rejected alternatives (don't re-propose)

- Python (`faster-whisper` + `pynput`) — a second system to install and keep
  working, for no gain. C++ talks to the Windows APIs this app is made of
  directly.
- Wake-word / always-on VAD as the *primary* trigger — needs a model
  (openWakeWord/Porcupine) to replace a button already in hand.
- NVIDIA Nemotron — NeMo/CoreML-first, thin Windows tooling. Parakeet is no
  longer in this list: the pinned whisper.cpp builds `src/parakeet.cpp`
  in-tree and CMakeLists already names the target. Unevaluated here, but the
  "thin Windows tooling" objection is dead (checked 2026-09-05).
- FluidVoice (the macOS app this imitates) — pure Swift, nothing reusable.
- `WH_KEYBOARD_LL` hook — an AVRCP press never reaches it; hence SMTC.

## Running

- **A running app blocks the build** — Windows locks the .exe, so the link
  step fails. `tasklist | grep -i talktoclaude`, then `taskkill //PID <pid>
  //F`. Say so before killing it; dictation may be in progress.
- `setup.bat` [clean] — finds VS Build Tools via `vswhere`, builds with
  CMake/NMake, fetches the model, stages exe + whisper/ggml DLLs into
  `bin/`. Incremental: `build/` is kept, so a source edit rebuilds in ~30s
  against 4m30s from scratch (measured `[DESKTOP]` 2026-09-05; most of the
  30s is vcvarsall plus CMake reconfigure, not compilation). `setup.bat
  clean` deletes `build/` first — needed when the whisper.cpp pin moves or
  the toolchain changes. Batch files here need CRLF — cmd.exe mis-parses
  LF-only.
- `run.bat [model path]` — defaults to `models\ggml-base.en.bin`.
- `run-small.bat` / `run-turbo.bat` — run.bat with `small.en` /
  `large-v3-turbo`, fetching the model on first use (setup.bat only ships
  base.en; the fetch goes to `.part` then renames, so a truncated download
  never looks like a model).
- Close the console or Ctrl+C to quit.
- `logs/` (gitignored): `talktoclaude-<stamp>.log` per run, plus setup logs.

## Code layout

**~1500 lines across six .cpp files. Read all of them before diagnosing
anything.** The symptom prints in one file and is caused in another: a
doubled transcript printed by the transcriber came from silence the capture
left on the end, a 35s stall came from what the capture handed over, and
reading only where the output appeared produced four wrong answers in a row
(2026-09-05) before a full read found the real defects — which were in files
no symptom pointed at.

- `CMakeLists.txt` — FetchContent whisper.cpp (pinned), links
  ole32/user32/winmm/windowsapp, copies DLLs next to the exe.
- `src/audio_capture.{h,cpp}` — WASAPI capture. Assumes IEEE float mix format.
  `CoInitializeEx(COINIT_MULTITHREADED)` on the main thread; `trigger.cpp`'s
  WinRT init **must** match this apartment or it throws `RPC_E_CHANGED_MODE`.
- `src/trigger.{h,cpp}` — SMTC trigger. Accepts both `Play` and `Pause` (no
  combined enum member). Logs the button value *before* filtering, so a
  delivered-but-unhandled press is still visible. Keeps the raw-input and
  call-control press probes; the `WH_KEYBOARD_LL` probe is gone, and must
  not come back: the system's raw input thread blocks on a low-level hook,
  and this thread owns one while sitting inside transcription and
  `SendInput`, so it stalled all input for the length of every injected
  transcript.
- `src/transcriber.{h,cpp}` — whisper wrapper. Shrinks `wparams.audio_ctx` to
  fit the audio, which is the 5.5x win over the fixed 30s mel window; floor is
  `kMinAudioCtx = 256`, a judgement value whose accuracy was never evaluated.
  Logs each segment with its timestamps to the log file, which is what tells
  a doubled decode apart from someone actually saying it twice. Deliberately
  does no dedupe — text-level "collapse the repeat" heuristics eat real
  speech ("go go" → "go").
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
  re-judge in a quiet room before drawing model conclusions. turbo is
  unusable on CPU and stays out of the running.
- Transcription is CPU-only, and that is the project's scope, not a gap.
  whisper.cpp is built without a GPU backend (`no GPU found`,
  `backends = 1`, `use gpu = 1` inert), so the RX 9070 XT idles. Adding a
  Vulkan backend would be a separate project; don't propose it as the next
  step here. On CPU, base.en at 0.4s per utterance is fast enough.
- whisper's non-speech markers (`[BLANK_AUDIO]`, `[ Silence ]`) get typed and
  submitted like any other transcript. Deliberate, but rarer now: a capture
  that never crossed `kSpeechThreshold` is dropped before the decoder and
  reports `[nothing said - peak ...]` on the console instead, so a typed
  marker means whisper heard *something* and made nothing of it.
- `base.en` accuracy on short HFP utterances is mediocre: live example,
  "then commit and close" → "the milk and clothes" (2.6s utterance). Fuel
  for the quiet-room `small.en` re-evaluation above.
- Silence is trimmed at the edges of an utterance but not inside one;
  language hardcoded to "en".
- Transcription is synchronous and blocks the trigger's message loop, but a
  press during it is *queued*, not lost: SMTC `ButtonPressed` fires on a
  WinRT threadpool thread and is posted into the loop. It used to be handled
  directly on that threadpool thread — which meant a press mid-transcription
  ran a second `whisper_full` concurrently on the same context, and on
  `[LAPTOP]` (2026-08-31) that hit ggml's `!isnan(sumf)` assert and killed
  the process. The same session showed one 3.1s recording stuck 35s+ inside
  `whisper_full` (why the press-during-transcription window was open at
  all). That recording was **silence**, which is the fallback ladder's
  pathological input: every temperature fails the no-speech check, so it
  pays all six decodes to arrive at `[BLANK_AUDIO]`. main.cpp now drops a
  capture whose peak never reached `kSpeechThreshold` instead of decoding
  it. That gate only ever fires on the two paths that bypass `sawSpeech` —
  the 30s cap, and a second button press (which `[LAPTOP]`'s Intel stack
  delivers mid-SCO, and is how a 3.1s silent recording got stopped and sent
  to whisper in the first place). A trailing-silence auto-stop can't reach
  it: `sawSpeech` tests the same threshold, so that path implies speech.
- **Doubled transcripts came from the trailing silence, not the decoder.**
  The auto-stop waits out ~1.5s of silence, so every capture ended with it,
  and whisper splits a capture into decode windows: the speech is one
  segment, the trailing silence is a second. Given ~1.5s of nothing the model
  hallucinates, and what it usually hallucinates is the sentence it just
  decoded — `[LAPTOP]` 2026-09-05, one 5.4s capture, segment 0 `[0..400]` and
  segment 1 `[400..600]` both "I need a new page for the stationary."
  whisper.cpp *already* clears the decoder's prompt history before a short
  final window (whisper.cpp:7153), so it was never prompt carryover; the
  window shouldn't exist. `stop()` now trims it. Do not re-diagnose this as a
  temperature/ladder problem — that was the previous wrong answer, and
  `temperature_inc = 0` is a fix for neither.
- `temperature_inc` was set to `0` for the 35s hang above and that was the
  wrong lever: with no retry to escalate to, the entropy/logprob check can
  reject a repetition-looped decode but not replace it, so the loop gets
  typed — "What about this? What about this?" for one spoken phrase. It is
  `0.4` now (temperatures 0.0/0.4/0.8, three decodes worst case), and with
  silence gated out beforehand the ladder no longer has anything slow to
  chew on.
- Logging is mutexed: a line is a timestamp write plus a body write, and the
  capture thread and trigger loop both log. Interleaving was possible but
  never observed in a log.
- The resampler carries its read position and last sample across packets.
  Restarting them per packet was only lossless because 48kHz gives a ratio
  of exactly 3; a 44.1kHz capture device would have drifted short and
  clicked at every seam. No such device has been used here, so the current
  code is reasoned, not measured.
- `SendInput`'s return value is checked. It stops at the first rejected
  event under a higher-integrity foreground window, and a silently dropped
  transcript is indistinguishable from whisper hearing nothing.
- `n_threads` is `hardware_concurrency()` (32 here). Never A/B'd against a
  smaller value, but the 0.4s measurement suggests it isn't hurting.
