#include "paths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include "logging.h"

namespace {

const wchar_t* const kModelFile = L"ggml-base.en.bin";
const wchar_t* const kModelHost = L"huggingface.co";
const wchar_t* const kModelPath = L"/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin";

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes,
                        nullptr, nullptr);
    return out;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool isDir(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return L".";
    std::wstring w(buf, n);
    const size_t slash = w.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : w.substr(0, slash);
}

// Resolves ..\ and friends so the logged path reads as a real location.
std::wstring fullPath(const std::wstring& path) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH, buf, nullptr);
    return (n == 0 || n >= MAX_PATH) ? path : std::wstring(buf, n);
}

// %LOCALAPPDATA%\talktoclaude\<sub>, created. Empty on failure.
std::wstring appDataDir(const wchar_t* sub) {
    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base))) {
        return {};
    }
    std::wstring dir = std::wstring(base) + L"\\talktoclaude";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\";
    dir += sub;
    CreateDirectoryW(dir.c_str(), nullptr);
    return isDir(dir) ? dir : std::wstring{};
}

// The repo layout's <dir> beside bin\, if this exe is running from a
// checkout. The checkout is recognised by setup.bat one level up, not by the
// folder itself: "..\logs exists" matched %TEMP%\logs when the exe was run
// from a Temp subfolder, and the log went there.
std::wstring repoDir(const wchar_t* sub) {
    const std::wstring root = fullPath(exeDir() + L"\\..");
    if (!exists(root + L"\\setup.bat")) return {};
    const std::wstring dir = root + L"\\" + sub;
    CreateDirectoryW(dir.c_str(), nullptr);
    return isDir(dir) ? dir : std::wstring{};
}

// Plain WinHTTP GET to a file, following redirects (huggingface hands off to
// a CDN), with a percentage on the console. Writes to <dest>.part and
// renames on success so a truncated download never looks like a model.
bool download(const std::wstring& dest) {
    const std::wstring part = dest + L".part";
    bool ok = false;

    HINTERNET session = WinHttpOpen(L"talktoclaude", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, kModelHost, INTERNET_DEFAULT_HTTPS_PORT, 0)
                                : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", kModelPath, nullptr,
                                                     WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     WINHTTP_FLAG_SECURE)
                                : nullptr;
    FILE* file = nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        wchar_t lengthText[32] = {};
        size = sizeof(lengthText);
        unsigned long long total = 0;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                lengthText, &size, WINHTTP_NO_HEADER_INDEX)) {
            total = _wcstoui64(lengthText, nullptr, 10);
        }

        if (status != 200) {
            Log::error("[model] download failed: HTTP %lu\n", status);
        } else if (_wfopen_s(&file, part.c_str(), L"wb") != 0 || !file) {
            Log::error("[model] can't write %s\n", toUtf8(part).c_str());
        } else {
            std::vector<char> buf(1 << 16);
            unsigned long long received = 0;
            int lastPercent = -1;
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(request, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                    Log::error("[model] read failed (0x%08lx)\n", GetLastError());
                    ok = false;
                    break;
                }
                if (got == 0) break;
                if (fwrite(buf.data(), 1, got, file) != got) {
                    Log::error("[model] write failed\n");
                    ok = false;
                    break;
                }
                received += got;
                if (total) {
                    const int percent = static_cast<int>(received * 100 / total);
                    if (percent != lastPercent) {
                        printf("\r[model] %d%% of %llu MB", percent, total >> 20);
                        fflush(stdout);
                        lastPercent = percent;
                    }
                }
            }
            printf("\n");
            if (ok && total && received != total) {
                Log::error("[model] short download: %llu of %llu bytes\n", received, total);
                ok = false;
            }
        }
    } else {
        Log::error("[model] request failed (0x%08lx)\n", GetLastError());
    }

    if (file) fclose(file);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);

    if (ok) ok = MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

}  // namespace

namespace Paths {

std::string logDir() {
    std::wstring dir = repoDir(L"logs");
    if (dir.empty()) dir = appDataDir(L"logs");
    if (dir.empty()) dir = fullPath(exeDir());
    return toUtf8(dir);
}

std::string defaultModel() {
    const std::wstring repo = repoDir(L"models");
    if (!repo.empty() && exists(repo + L"\\" + kModelFile)) return toUtf8(repo + L"\\" + kModelFile);

    const std::wstring data = appDataDir(L"models");
    if (data.empty()) {
        Log::error("[model] no writable models folder\n");
        return {};
    }
    const std::wstring model = data + L"\\" + kModelFile;
    if (exists(model)) return toUtf8(model);

    Log::info("[model] first run: fetching base.en (142 MB, once) into %s\n",
              toUtf8(data).c_str());
    if (!download(model)) return {};
    return toUtf8(model);
}

}  // namespace Paths
