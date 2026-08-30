#include "logging.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

FILE* g_file = nullptr;
std::string g_path;

std::string exeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return ".";
    std::wstring w(buf, n);
    const size_t slash = w.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return ".";
    w.resize(slash);
    std::string out(w.size() * 2, '\0');
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        out.data(), static_cast<int>(out.size()), nullptr,
                                        nullptr);
    out.resize(len < 0 ? 0 : len);
    return out;
}

// Writes to the log file with a HH:MM:SS.mmm prefix. `console` gets the raw
// line without the prefix, so the terminal keeps looking like it did.
void emit(FILE* console, const char* fmt, va_list args) {
    if (console) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(console, fmt, copy);
        va_end(copy);
        fflush(console);
    }
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(g_file, fmt, args);
    fflush(g_file);
}

}  // namespace

namespace Log {

bool init() {
    // The exe lives in bin/; logs go next to it in the repo root's logs/.
    std::string dir = exeDir() + "\\..\\logs";
    char full[MAX_PATH];
    if (GetFullPathNameA(dir.c_str(), MAX_PATH, full, nullptr)) dir = full;
    CreateDirectoryA(dir.c_str(), nullptr);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    snprintf(name, sizeof(name), "\\talktoclaude-%04d%02d%02d-%02d%02d%02d.log", t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_path = dir + name;

    g_file = fopen(g_path.c_str(), "w");
    if (!g_file) {
        g_path.clear();
        return false;
    }
    fprintf(g_file, "=== talktoclaude session started %04d-%02d-%02d %02d:%02d:%02d ===\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fflush(g_file);
    return true;
}

void close() {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(stdout, fmt, args);
    va_end(args);
}

void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(stderr, fmt, args);
    va_end(args);
}

void fileOnly(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(nullptr, fmt, args);
    va_end(args);
}

const std::string& path() { return g_path; }

}  // namespace Log
