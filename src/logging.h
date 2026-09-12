#pragma once

#include <string>

// File logging. The exe is a windows-subsystem app with no console, so
// everything lands in a timestamped file in `dir` (see Paths::logDir);
// follow it live with `Get-Content <file> -Wait`.
namespace Log {

// Opens <dir>\talktoclaude-YYYYMMDD-HHMMSS.log. Safe to skip checking the
// result: on failure logging silently degrades to nothing.
bool init(const std::string& dir);
void close();

// Two names for the same sink, kept so a grep for `error` finds the ones
// that mattered.
void info(const char* fmt, ...);
void error(const char* fmt, ...);

const std::string& path();

}  // namespace Log
