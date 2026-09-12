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
- **Two copies can run at once, briefly, without harm.** Building is safe
  with the installed app running (separate files), and so is a short smoke
  test of a dev build: while both are alive the media session bounces
  between them every 3s and a press would be typed twice, and the moment
  one exits the other is sole owner again. Nothing persists. Smoke-test
  freely; just don't leave two running. The v0.2.0 mutex blocks a second
  guarded instance, not an older build.
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
- Quit from the tray menu. `run.bat` launches the tray app too; add
  `--console` to see output while developing. A running app still blocks
  the build, and now it has no window to remind you: `tasklist`.
- `logs/` (gitignored): `talktoclaude-<stamp>.log` per run, plus setup logs.
- **The batch files are dev tooling and don't ship.** The product is the
  bare exe (`src/paths.cpp`): with no argument it looks for
  `..\models\ggml-base.en.bin` when running from a checkout (recognised by
  `..\setup.bat`), else `%LOCALAPPDATA%\talktoclaude\models`, downloading
  it there over WinHTTP on first run; logs follow the same rule. Verified
  2026-09-05 from a Temp folder: 148 MB fetched, model loaded, log landed in
  LocalAppData. Don't detect the checkout by "..\logs exists" — that matched
  `%TEMP%\logs` and put the log there.
- **Shipping:** `.github/workflows/build.yml` builds x64 and arm64 on
  windows-latest (Ninja, Release), zips each, and runs Inno Setup on
  `installer/talktoclaude.iss` for a per-user installer that adds the
  install dir to the user PATH. A `v*` tag attaches all four to a release.
  `pages.yml` publishes `site/` (landing page with a replayed demo) to
  https://frenchcommando.github.io/talktoclaude/, which reads the latest
  release from the GitHub API for its download button. First release
  v0.1.0 on 2026-09-05; MIT licensed since 2026-09-06 (v0.1.0's assets
  predate the LICENSE file). The installer has been built in CI only, never
  run on a real machine; the zip's exe was (see the paths bullet).
- **arm64 is cross-compiled and needs three things** (each found by a red
  CI run, 2026-09-05): `CMAKE_SYSTEM_PROCESSOR=ARM64` or ggml compiles its
  x86 kernels for an ARM link; `clang-cl` with `--target=arm64-pc-windows-
  msvc` because ggml refuses MSVC for ARM outright; and `/EHsc` re-added
  once `CMAKE_CXX_FLAGS` is overridden. clang-cl also enforces C++/WinRT's
  rule that `<unknwn.h>` precede its headers when a classic COM interface
  is used, hence the include at the top of trigger.h. Never run on arm64
  hardware.
- **CI builds ggml with `GGML_NATIVE=OFF` and AVX2/FMA/F16C pinned.** The
  default (native) compiled for the runner's AVX-512 CPU, and v0.2.0
  crashed on `[LAPTOP]` with `0xC000001D` in ggml-cpu.dll the moment the
  model loaded (Application event log, 2026-09-06 15:38). The log file
  ends after `whisper_backend_init_gpu` with no "ready", which is the
  signature. v0.2.1 fixed it; the CI zip was run on `[LAPTOP]` to confirm.
  Local `setup.bat` builds stay native and are fine on their own machine.
- **The CRT is linked statically** (`CMAKE_MSVC_RUNTIME_LIBRARY` in
  CMakeLists.txt, inherited by whisper/ggml). Up to v0.2.7 every shipped
  binary imported VCRUNTIME140/MSVCP140/api-ms-win-crt-*, and the winget
  validator's clean VM (no VC++ redist) reported `STATUS_DLL_NOT_FOUND`
  on launch (PR comment 2026-09-11 UTC). Never showed on `[DESKTOP]` or
  `[LAPTOP]` because both already had the redist. Verified locally
  2026-09-10: no CRT DLL names in any staged binary, exe grew 1.6→2.9 MB.
  Shipped as v0.2.8 (2026-09-11) — and the validator failed again with
  the same status, because `dumpbin /DEPENDENTS` on the 0.2.8 zip showed
  ggml-base.dll and ggml-cpu.dll importing **VCOMP140.DLL**: the OpenMP
  runtime is redist-only and the static CRT does not cover it. Fixed
  2026-09-12 with `GGML_OPENMP=OFF` (ggml's own threadpool; `n_threads`
  still honoured). Check `dumpbin /DEPENDENTS` on *every* staged DLL, not
  just the exe, before calling a dependency fixed. The same rebuild
  exposed a staging gap: the DLL copy was a POST_BUILD step on the exe,
  so a ggml-only change left stale DLLs in build\ and bin\; it is an
  `ALL` custom target now. Local `setup.bat` builds are `Debug`
  (CMakeCache `CMAKE_BUILD_TYPE=Debug`, DLLs import `VCOMP140D`) — CI
  is Release; the timings in this file came from the Debug build.
- **Signing:** the exe is unsigned, so SmartScreen warns on first run.
  Deliberate for now; the fix costs a certificate.
- **winget:** submitted 2026-09-06 as `FrenchCommando.talktoclaude` 0.2.6,
  https://github.com/microsoft/winget-pkgs/pull/430607, from the fork
  `FrenchCommando/winget-pkgs`; bumped in place to 0.2.7 on 2026-09-07
  and to 0.2.8 on 2026-09-11 (same PR, same branch
  `FrenchCommando.talktoclaude-0.2.6`, one commit swapping the version
  folder, PR retitled). The fork checkout at `C:\Users\marti\winget-pkgs`
  is a full 671k-file clone whose working tree is partly missing; stage
  only the package folder (`git rm -r 0.2.x`, `git add 0.2.y`) and never
  `git add -A` there. While the new-package PR is
  unmerged, re-tagging means this: edit the open PR, don't open another,
  and it keeps its queue position. Three hand-written manifests (version,
  installer, en-US locale; schema 1.12.0), `winget validate` passed.
  winget's own download skips the browser and SmartScreen prompts, so it
  is the clean install path for the terminal crowd until the Store. **Per
  release, automated:** the `winget` job at the end of build.yml runs
  `vedantmgoyal9/winget-releaser` after the release assets are up and
  opens the update PR from the fork. It is a job in build.yml, not an
  `on: release` workflow, because the release is created with
  `GITHUB_TOKEN` and events from that token never trigger workflows. It
  needs the `WINGET_TOKEN` repo secret: a *classic* PAT with `public_repo`
  (fine-grained tokens aren't supported by the action), created and set by
  the user (`gh secret set WINGET_TOKEN`) — **set 2026-09-06 evening** (`gh` shows 2026-09-07T02:04Z; GitHub timestamps are UTC, local is UTC-4),
  check with `gh secret list` before claiming it's missing. It only works
  once a first version exists in winget-pkgs, so the new-package PR must
  merge before the job can succeed (v0.2.7's run failed on cue); until
  then the job simply fails and nothing else is affected.
  **Manual fallback:** copy the three files to a new version
  folder, bump `PackageVersion`, the two `InstallerUrl`s, `ReleaseDate`,
  `ReleaseNotesUrl`, and the two `InstallerSha256` (uppercase, from
  `sha256sum` on the release assets), then a PR from a branch of the fork
  titled "Update: FrenchCommando.talktoclaude version x.y.z". The first
  validation run labelled it `Validation-Executable-Error` (couldn't find
  a primary executable: per-user LocalAppData install, tray app, no
  window). Fixed in the manifest rather than argued in a comment: the
  installer manifest carries an `InstallationMetadata` block naming
  `%LOCALAPPDATA%\Programs\talktoclaude` and `talktoclaude.exe` as the
  launch file. winget-releaser carries existing installer fields forward,
  so updates keep it; check the generated PR the first time. **Expect the
  first merge to take weeks, not days.** Measured 2026-09-07 over the
  last 30 merged New-Package PRs: median 15 days, quarter under 13,
  quarter over 24, worst about 2 months; 1,211 new-package PRs were open.
  The human review is the bottleneck. Updates skip it and merge in hours.
  Measure again with `gh api search/issues?q=repo:microsoft/winget-pkgs+
  is:pr+is:merged+label:New-Package` if it matters. Releases up to
  0.2.6 register in Apps as "talktoclaude version 0.2.6" (Inno's default),
  so `winget uninstall talktoclaude` finds nothing there; use the full
  name or `--id "{7C2B1B0E-6C3B-4F3E-9D3A-talktoclaude}_is1"`. The .iss
  sets `UninstallDisplayName=talktoclaude` from 0.2.7 on.
- **TODO (pinned 2026-09-06): Microsoft Store.** Chosen over SignPath
  Foundation (free but the publisher name would be theirs) because Store
  installs skip SmartScreen entirely. Blocked on the developer account: a
  previous signup on another project failed on home-network proxy issues
  during identity verification; retry on a plain connection. **Done ahead
  of the account (2026-09-06):** `msix/AppxManifest.xml` (full trust,
  microphone capability, app execution alias `talktoclaude` replacing the
  PATH edit), logo PNGs from `tools/make_icon.py` into `msix/Assets`, CI
  packs an *unsigned* `.msix` per arch with `makeappx` into `dist/msix`
  (workflow artifact only, not a release asset — unsigned can't be
  installed by users; the Store signs it), and `site/privacy.html` for
  the microphone-capability requirement. Packed locally to validate the
  manifest. **Still to do once the account exists:** reserve the app name
  in Partner Center and put its real Identity Name/Publisher into the
  manifest (placeholders now), upload the artifact, and *test whether
  `Tray::promote()` still reaches Explorer* under MSIX registry
  virtualisation — it probably won't; fallback is the Settings switch.
  Store and GitHub releases coexist; the mutex works across both. Every
  release then goes through Store certification (a day or three); the
  submission API can automate uploads later.
- **Site (2026-09-06):** hype copy at the user's request, stat tiles, and
  an SVG stick-figure scene ("Sticky", the user's name for him) driven by
  the same replay script as
  the console demo — phases press/speak/think/type/done toggle a class on
  the svg root. The demo conversation is fiction by request and the page
  says so; the "milk and clothes" mishear in it is real. The one "review"
  is labelled as not real. Keep it that way: no fabricated testimonials.

## Code layout

**~1900 lines across eight .cpp files. Read all of them before diagnosing
anything.** The symptom prints in one file and is caused in another: a
doubled transcript printed by the transcriber came from silence the capture
left on the end, a 35s stall came from what the capture handed over, and
reading only where the output appeared produced four wrong answers in a row
(2026-09-05) before a full read found the real defects — which were in files
no symptom pointed at.

- `CMakeLists.txt` — FetchContent whisper.cpp (pinned), links
  ole32/user32/winmm/windowsapp/shell32/winhttp, copies DLLs next to the
  exe. `LANGUAGES CXX RC` for the icon resource; `WIN32_EXECUTABLE` with
  `/ENTRY:mainCRTStartup` for the tray app.
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
- `src/tray.{h,cpp}` — notification-area icon: discs drawn at runtime (no
  resources), four states, balloon, right-click menu. Owned by trigger.cpp's
  hidden window; `Trigger::start()` creates window + icon + SMTC before
  main loads the model so the icon shows "loading" through a first-run
  download, `run()` pumps. Either click opens the menu; left-click used to
  act as a press and was removed the same day because it can't work:
  clicking the icon focuses the taskbar, so the window captured for
  injection is the taskbar and the transcript has nowhere to go. Only a
  trigger that leaves focus alone (the headset button, a media key) can
  start listening. `Tray::promote()` sets `IsPromoted=1` on the app's
  entry under `HKCU\Control Panel\NotifyIconSettings` (matched by
  `ExecutablePath`) so Windows 11 keeps the icon in the corner instead of
  the overflow; undocumented key, retried on the 3s timer up to 10 times
  because Explorer creates the entry after the icon appears, and the
  fallback is the Settings page. Workaround, not an API. The exe is `WIN32_EXECUTABLE` with `/ENTRY:mainCRTStartup`; there
  is no console unless `--console`, which `AttachConsole`s the parent
  terminal or allocates one, and `Log::setConsole(true)`. Consequence for
  scripts: `--help` output goes to the attached console, not a pipe, so CI
  checks the exit code only. Single instance via a named mutex.
- `src/talktoclaude.rc` + `talktoclaude.ico` — the app icon (Explorer,
  Start menu, installer): dark rounded tile, red dot, two arcs, same
  design as `site/icon.svg`. The .ico is generated by
  `tools/make_icon.py` (stdlib only, 16 to 256 px, 32-bit BMP frames);
  regenerate it there if the design changes rather than editing the
  binary. The tray keeps its runtime-drawn state discs.
- `src/logging.{h,cpp}` — console + file; `Log::fileOnly` for whisper's chatter.
- `src/paths.{h,cpp}` — log dir, default model lookup, first-run download.
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
- Speech detection is a single-sample peak test, so one click counts as
  speech: it sets `sawSpeech`, the 1.5s timer runs out, and the capture
  ends with ~50ms of "speech" in it. Observed `[LAPTOP]` 2026-09-05 as
  `captured 4.9s, kept 0.6s` (0.6 = 300ms lead + 250ms tail + the click),
  during button fiddling rather than dictation. A windowed RMS would fix it
  but changes what the 0.01 threshold means and needs re-tuning on the
  buds; not done because it has never typed junk during real use.
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
