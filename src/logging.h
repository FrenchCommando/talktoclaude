#pragma once

#include <string>

// Console + file logging. Everything the app prints also lands in a
// timestamped file under <repo>/logs/ (a sibling of the exe's bin/ dir), so
// there's a record of a session after the console window is gone.
namespace Log {

// Opens logs/talktoclaude-YYYYMMDD-HHMMSS.log. Safe to skip checking the
// result: on failure logging silently degrades to console-only.
bool init();
void close();

// Both go to the log file; info also to stdout, error also to stderr.
void info(const char* fmt, ...);
void error(const char* fmt, ...);

// Log file only — for firehose output (whisper's own diagnostics) that would
// bury the console.
void fileOnly(const char* fmt, ...);

const std::string& path();

}  // namespace Log
