#include "trigger.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.Devices.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <systemmediatransportcontrolsinterop.h>

#include "logging.h"

using namespace winrt;
using namespace winrt::Windows::Media;

namespace {

// Ctrl+Alt+V re-claims the SMTC session when another app has taken the media
// button. Registered process-wide, so it works whatever has focus.
constexpr int kReclaimHotkeyId = 1;
constexpr UINT kReclaimHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kReclaimHotkeyVk = 'V';

// ...and the claim is re-asserted on a timer anyway, so the button belongs
// to this app for as long as it's running. The cost is deliberate: while
// talktoclaude is open the headset can't pause YouTube, because both can't
// own the session.
constexpr UINT kReclaimIntervalMs = 3000;

// Thread message posted by requestStop() (from the capture thread's
// auto-stop) into the trigger's message loop.
constexpr UINT kStopRequestMessage = WM_APP + 1;

// Thread message posted by onButtonPressed(). SMTC's ButtonPressed fires on
// a WinRT threadpool thread, not the message loop — handling the toggle in
// the event handler ran capture/transcription concurrently with whatever the
// loop was doing. Two whisper_full calls raced on one whisper_context and
// died on ggml's NaN assert. Marshal every press here instead, so the loop
// thread is the only one that touches capture, transcriber, and recording_.
constexpr UINT kButtonToggleMessage = WM_APP + 2;

// Who currently owns the "now playing" session — that is, where a
// hardware media button's press gets delivered. There's no way to *force*
// the session, but Windows.Media.Control can report it. If this never
// names talktoclaude, our own registration isn't landing and AVRCP presses
// have nothing to route to — exactly the failure the interop rewrite fixed.
std::string currentSessionOwner() {
    using namespace winrt::Windows::Media::Control;
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = manager.GetCurrentSession();
        if (!session) return "(no session at all)";
        return winrt::to_string(session.SourceAppUserModelId());
    } catch (winrt::hresult_error const& error) {
        return "(query failed: " + winrt::to_string(error.message()) + ")";
    }
}

// --- press probe -----------------------------------------------------------
// The buds expose a Bluetooth HID node (service 1124) that neither the
// laptop nor the OnePlus has, and in hands-free mode their button may be
// remapped from an AVRCP play/pause into a HID consumer-control or
// call-control event — which SMTC never sees, so "zero smtc lines" does not
// mean "zero presses sent". This probe watches the other channels: raw-input
// HID reports on the consumer (0x0C) and telephony (0x0B) usage pages.
// Deliberately narrow: ordinary typing is never examined or logged.

LRESULT CALLBACK probeWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INPUT) {
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER));
        std::vector<uint8_t> bytes(size);
        if (size > 0 &&
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, bytes.data(), &size,
                            sizeof(RAWINPUTHEADER)) == size) {
            const auto* raw = reinterpret_cast<const RAWINPUT*>(bytes.data());
            if (raw->header.dwType == RIM_TYPEHID) {
                const BYTE* data = raw->data.hid.bRawData;
                const DWORD total = raw->data.hid.dwSizeHid * raw->data.hid.dwCount;
                std::string hex;
                for (DWORD i = 0; i < total && i < 32; ++i) {
                    char byte[4];
                    snprintf(byte, sizeof(byte), "%02x ", data[i]);
                    hex += byte;
                }
                Log::info("[probe] HID report (%lu bytes): %s\n",
                          static_cast<unsigned long>(total), hex.c_str());
            }
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Hidden top-level window: the SMTC interop registration needs an HWND, and
// the raw-input probe needs a sink window. Never shown.
HWND createHiddenWindow() {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = probeWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"talktoclaude_window";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"talktoclaude", WS_OVERLAPPED,
                                  0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) Log::error("[trigger] couldn't create hidden window\n");
    return window;
}

void startPressProbe(HWND window) {
    RAWINPUTDEVICE devices[] = {
        {0x0C, 0x01, RIDEV_INPUTSINK, window},  // consumer control (media buttons)
        {0x0B, 0x05, RIDEV_INPUTSINK, window},  // telephony headset (call controls)
    };
    if (RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        Log::info("[probe] watching consumer/telephony HID reports and media-range keys\n");
    } else {
        Log::error("[probe] RegisterRawInputDevices failed (0x%08lx)\n", GetLastError());
    }
}

// --- call-control probe ----------------------------------------------------

winrt::Windows::Foundation::IReference<winrt::guid> containerOf(
    winrt::Windows::Devices::Enumeration::DeviceInformation const& device) {
    return device.Properties()
        .TryLookup(L"System.Devices.ContainerId")
        .try_as<winrt::Windows::Foundation::IReference<winrt::guid>>();
}

// Container (physical device) of the default capture device, or nullptr.
winrt::Windows::Foundation::IReference<winrt::guid> defaultCaptureContainer() {
    using namespace winrt::Windows::Devices::Enumeration;
    using namespace winrt::Windows::Media::Devices;
    try {
        const winrt::hstring captureId =
            MediaDevice::GetDefaultAudioCaptureId(AudioDeviceRole::Default);
        if (captureId.empty()) return nullptr;
        const auto capture =
            DeviceInformation::CreateFromIdAsync(captureId, {L"System.Devices.ContainerId"}).get();
        return containerOf(capture);
    } catch (winrt::hresult_error const&) {
        return nullptr;
    }
}

// CallControl.GetDefault() only looks at the *default communications
// device*, which need not be the headset — so also try FromId on every
// render endpoint that shares the capture device's container.
winrt::Windows::Media::Devices::CallControl armCallControlOnCaptureContainer() {
    using namespace winrt::Windows::Devices::Enumeration;
    using namespace winrt::Windows::Media::Devices;
    try {
        const auto captureContainer = defaultCaptureContainer();
        if (!captureContainer) return nullptr;
        const auto renders = DeviceInformation::FindAllAsync(MediaDevice::GetAudioRenderSelector(),
                                                             {L"System.Devices.ContainerId"})
                                 .get();
        for (const auto& render : renders) {
            const auto container = containerOf(render);
            if (!container || container.Value() != captureContainer.Value()) continue;
            try {
                auto control = CallControl::FromId(render.Id());
                if (control) {
                    Log::info("[probe] call-control armed via: %s\n",
                              winrt::to_string(render.Name()).c_str());
                    return control;
                }
            } catch (winrt::hresult_error const&) {
                // This endpoint doesn't do call control; try the next.
            }
        }
    } catch (winrt::hresult_error const& error) {
        Log::info("[probe] call-control arming failed: %s\n",
                  winrt::to_string(error.message()).c_str());
    }
    return nullptr;
}

} // namespace

Trigger::Trigger(Callback onPress, Callback onStopRequest)
    : onPress_(std::move(onPress)), onStopRequest_(std::move(onStopRequest)) {}

Trigger::~Trigger() {
    stop();
}

void Trigger::onButtonPressed(
    SystemMediaTransportControls const&,
    SystemMediaTransportControlsButtonPressedEventArgs const& args) {
    const auto button = args.Button();
    // Logged before the filter: a press that arrives as some other button was
    // previously discarded in silence, which is indistinguishable from the
    // press never arriving at all. Enum order is Play 0, Pause 1, Stop 2,
    // Record 3, FastForward 4, Rewind 5, Next 6, Previous 7.
    Log::info("[smtc button pressed: %d]\n", static_cast<int>(button));

    if (button != SystemMediaTransportControlsButton::Play &&
        button != SystemMediaTransportControlsButton::Pause) {
        return;
    }

    // This handler runs on a WinRT threadpool thread. Don't toggle here —
    // post to the message loop so presses serialize with transcription and
    // the auto-stop path. A press that lands mid-transcription is queued and
    // starts the next utterance once the loop is free, instead of running a
    // second transcription concurrently (which crashed on a ggml NaN assert).
    if (threadId_ != 0) PostThreadMessageW(threadId_, kButtonToggleMessage, 0, 0);
}

void Trigger::run() {
    // AudioCapture already CoInitializeEx's this thread as MTA; match that
    // apartment type here or WinRT's own init throws RPC_E_CHANGED_MODE.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    threadId_ = GetCurrentThreadId();

    HWND window = createHiddenWindow();

    // The canonical desktop-app SMTC registration: GetForWindow, metadata,
    // PlaybackStatus = Playing. This is what makes GetCurrentSession()
    // able to return us, which is what AVRCP presses route by.
    if (window) {
        try {
            auto interop = winrt::get_activation_factory<SystemMediaTransportControls,
                                                         ISystemMediaTransportControlsInterop>();
            winrt::check_hresult(interop->GetForWindow(
                window, winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(smtc_)));

            smtc_.IsEnabled(true);
            smtc_.IsPlayEnabled(true);
            smtc_.IsPauseEnabled(true);
            // Windows only delivers buttons that are enabled. Stop costs
            // nothing to accept and means a press arriving as Stop shows up
            // in the log instead of being dropped before we ever see it.
            smtc_.IsStopEnabled(true);
            buttonPressedToken_ = smtc_.ButtonPressed({this, &Trigger::onButtonPressed});

            // A session without display metadata may be ignored entirely.
            auto updater = smtc_.DisplayUpdater();
            updater.Type(MediaPlaybackType::Music);
            updater.MusicProperties().Title(L"talktoclaude");
            updater.Update();
            smtc_.PlaybackStatus(MediaPlaybackStatus::Playing);
            Log::info("[trigger] SMTC session registered via interop\n");
        } catch (winrt::hresult_error const& error) {
            Log::error("[trigger] SMTC registration failed: %s\n",
                       winrt::to_string(error.message()).c_str());
            smtc_ = nullptr;
        }
    }
    reportSessionOwner();

    // Thread-bound hotkey: WM_HOTKEY arrives in this loop, not a window proc.
    if (RegisterHotKey(nullptr, kReclaimHotkeyId, kReclaimHotkeyMods, kReclaimHotkeyVk)) {
        Log::info("Session re-asserted every %us; Ctrl+Alt+V forces it now.\n",
                  kReclaimIntervalMs / 1000);
    } else {
        Log::error("[trigger] couldn't register Ctrl+Alt+V (already taken?)\n");
    }

    reclaimTimerId_ = SetTimer(nullptr, 0, kReclaimIntervalMs, nullptr);
    if (reclaimTimerId_ == 0) Log::error("[trigger] SetTimer failed; no auto re-claim\n");

    // See the press-probe block at the top of this file. There is
    // deliberately no WH_KEYBOARD_LL hook alongside it: an AVRCP press never
    // reaches one (that is why this app uses SMTC at all), and a low-level
    // hook owned by this thread is actively harmful. The system's raw input
    // thread blocks on it, and this thread is inside the callback
    // (transcribing, or feeding SendInput) rather than pumping messages, so
    // every injected keystroke stalls all input until the hook times out.
    if (window) startPressProbe(window);

    // Call-control probe: in hands-free mode a headset's button is a call
    // button, not a media button — idle HFP typically maps it to "redial",
    // in-call to "hang up". Those arrive via Windows.Media.Devices.
    // CallControl (the Bluetooth Audio Gateway), not SMTC, not HID, not a
    // key — and are dropped unless someone subscribes. Subscribe to
    // everything and log. The object must outlive the message loop.
    winrt::Windows::Media::Devices::CallControl callControl{nullptr};
    try {
        callControl = winrt::Windows::Media::Devices::CallControl::GetDefault();
    } catch (winrt::hresult_error const& error) {
        Log::info("[probe] call-control unavailable: %s\n",
                  winrt::to_string(error.message()).c_str());
    }
    if (!callControl) callControl = armCallControlOnCaptureContainer();
    if (callControl) {
        callControl.AnswerRequested([](auto&&...) { Log::info("[probe] call-control: answer\n"); });
        callControl.HangUpRequested([](auto&&...) { Log::info("[probe] call-control: hang-up\n"); });
        callControl.RedialRequested([](auto&&...) { Log::info("[probe] call-control: redial\n"); });
        callControl.KeypadPressed([](auto&&...) { Log::info("[probe] call-control: keypad\n"); });
        callControl.AudioTransferRequested(
            [](auto&&...) { Log::info("[probe] call-control: audio transfer\n"); });
        Log::info("[probe] call-control armed\n");
    } else {
        Log::info("[probe] call-control: nothing to arm\n");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == kReclaimHotkeyId) {
            reclaim(true);
            continue;
        }
        if (msg.message == WM_TIMER && msg.wParam == reclaimTimerId_) {
            // File only: every 3s, and the console is the dictation output.
            reclaim(false);
            continue;
        }
        if (msg.message == kStopRequestMessage) {
            if (onStopRequest_) onStopRequest_();
            continue;
        }
        if (msg.message == kButtonToggleMessage) {
            if (onPress_) onPress_();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(nullptr, reclaimTimerId_);
    UnregisterHotKey(nullptr, kReclaimHotkeyId);
    if (smtc_ && buttonPressedToken_) {
        smtc_.ButtonPressed(buttonPressedToken_);
        buttonPressedToken_ = {};
    }
    if (smtc_) {
        smtc_.IsEnabled(false);
        smtc_ = nullptr;
    }
    if (window) DestroyWindow(window);
}

void Trigger::reclaim(bool announce) {
    if (!smtc_) return;
    // Windows gives the button to whichever session most recently started
    // playing; re-asserting Playing keeps us that session. A no-op when
    // nothing has taken it.
    smtc_.PlaybackStatus(MediaPlaybackStatus::Playing);
    reportSessionOwner();
    if (announce) Log::info("[session re-asserted]\n");
}

// Logged only when it changes: unchanged it's noise every 3s, but the moment
// it moves it names whatever took the button.
void Trigger::reportSessionOwner() {
    std::string owner = currentSessionOwner();
    if (owner == lastSessionOwner_) return;
    Log::info("[media button owned by: %s]\n", owner.c_str());
    lastSessionOwner_ = std::move(owner);
}

void Trigger::requestStop() {
    if (threadId_ != 0) PostThreadMessageW(threadId_, kStopRequestMessage, 0, 0);
}

void Trigger::stop() {
    if (threadId_ != 0) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        threadId_ = 0;
    }
}
