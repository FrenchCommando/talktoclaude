#include "tray.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cmath>
#include <cstring>
#include <cwchar>

namespace {

constexpr UINT kIconId = 1;

// A filled disc, drawn at runtime so the exe carries no resources. Colour
// per state; alpha outside the disc so it sits on any taskbar theme.
HICON makeIcon(COLORREF colour) {
    const int size = GetSystemMetrics(SM_CXSMICON) > 0 ? GetSystemMetrics(SM_CXSMICON) : 16;

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;  // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    const HDC screen = GetDC(nullptr);
    const HBITMAP colourBitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                                  DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colourBitmap || !bits) return nullptr;

    auto* pixels = static_cast<DWORD*>(bits);
    const float centre = size / 2.0f;
    const float radius = size / 2.0f - 1.0f;
    const DWORD r = GetRValue(colour), g = GetGValue(colour), b = GetBValue(colour);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x + 0.5f - centre, dy = y + 0.5f - centre;
            const float d = radius - static_cast<float>(sqrt(dx * dx + dy * dy));
            // Soft edge: alpha ramps over one pixel.
            const float coverage = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
            const DWORD a = static_cast<DWORD>(coverage * 255.0f);
            // Premultiplied, as DIB icons expect.
            pixels[y * size + x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        }
    }

    const HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colourBitmap;
    info.hbmMask = mask;
    const HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(colourBitmap);
    return icon;
}

HICON iconFor(Tray::State state) {
    static HICON idle = makeIcon(RGB(140, 140, 140));
    static HICON listening = makeIcon(RGB(220, 60, 50));
    static HICON transcribing = makeIcon(RGB(240, 180, 40));
    static HICON busy = makeIcon(RGB(70, 130, 220));
    switch (state) {
        case Tray::State::Listening: return listening;
        case Tray::State::Transcribing: return transcribing;
        case Tray::State::Busy: return busy;
        default: return idle;
    }
}

void toWide(const std::string& utf8, wchar_t* out, int capacity) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out, capacity - 1);
    out[n > 0 ? n - 1 : 0] = L'\0';
    out[capacity - 1] = L'\0';
}

NOTIFYICONDATAW base(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kIconId;
    return data;
}

}  // namespace

namespace Tray {

bool add(void* window) {
    NOTIFYICONDATAW data = base(static_cast<HWND>(window));
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = iconFor(State::Busy);
    wcscpy_s(data.szTip, L"talktoclaude");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}

void remove(void* window) {
    NOTIFYICONDATAW data = base(static_cast<HWND>(window));
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void set(void* window, State state, const std::string& tip) {
    NOTIFYICONDATAW data = base(static_cast<HWND>(window));
    data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = iconFor(state);
    toWide(tip, data.szTip, static_cast<int>(sizeof(data.szTip) / sizeof(wchar_t)));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void notify(void* window, const std::string& title, const std::string& text) {
    NOTIFYICONDATAW data = base(static_cast<HWND>(window));
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    toWide(title, data.szInfoTitle, static_cast<int>(sizeof(data.szInfoTitle) / sizeof(wchar_t)));
    toWide(text, data.szInfo, static_cast<int>(sizeof(data.szInfo) / sizeof(wchar_t)));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool promote() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return false;

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0,
                      KEY_READ, &root) != ERROR_SUCCESS) {
        return false;
    }
    bool done = false;
    wchar_t name[64];
    for (DWORD i = 0; !done; ++i) {
        DWORD nameLen = 64;
        if (RegEnumKeyExW(root, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        HKEY entry = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_SET_VALUE, &entry) != ERROR_SUCCESS) {
            continue;
        }
        wchar_t path[MAX_PATH * 2];
        DWORD size = sizeof(path);
        DWORD type = 0;
        if (RegQueryValueExW(entry, L"ExecutablePath", nullptr, &type,
                             reinterpret_cast<BYTE*>(path), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && _wcsicmp(path, exe) == 0) {
            const DWORD one = 1;
            done = RegSetValueExW(entry, L"IsPromoted", 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&one), sizeof(one)) ==
                   ERROR_SUCCESS;
        }
        RegCloseKey(entry);
    }
    RegCloseKey(root);
    return done;
}

unsigned showMenu(void* window) {
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpenLogs, L"Open log folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit talktoclaude");
    POINT cursor;
    GetCursorPos(&cursor);
    // The documented dance: the menu only dismisses on an outside click if
    // our window is foreground, and needs a WM_NULL after to close cleanly.
    SetForegroundWindow(static_cast<HWND>(window));
    const unsigned chosen = static_cast<unsigned>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0,
        static_cast<HWND>(window), nullptr));
    PostMessageW(static_cast<HWND>(window), WM_NULL, 0, 0);
    DestroyMenu(menu);
    return chosen;
}

}  // namespace Tray
