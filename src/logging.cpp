#include "logging.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {

FILE* g_file = nullptr;
std::string g_path;

// The capture thread, the trigger's message loop, and whisper's worker
// threads all log. fprintf locks per-call, so nothing corrupts, but a line
// here is a timestamp prefix plus a body in two calls — without this those
// two interleave with another thread's and the log stops being readable
// exactly when several threads are busy, which is when it's worth reading.
std::mutex g_mutex;

// Writes to the log file with a HH:MM:SS.mmm prefix. `console` gets the raw
// line without the prefix, so the terminal keeps looking like it did.
void emit(FILE* console, const char* fmt, va_list args) {
    const std::lock_guard<std::mutex> lock(g_mutex);
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

bool init(const std::string& dir) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    snprintf(name, sizeof(name), "\\talktoclaude-%04d%02d%02d-%02d%02d%02d.log", t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_path = dir + name;

    // The path is UTF-8; fopen would take it as the ANSI code page.
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, g_path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(wideLen > 0 ? wideLen : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, g_path.c_str(), -1, wide.data(), wideLen);
    g_file = _wfopen(wide.c_str(), L"w");
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
