#pragma once

#include <string>

// Where things live. Two layouts:
//
//  - Repo: bin\talktoclaude.exe with ..\logs and ..\models beside it. Used by
//    setup.bat / run.bat. Detected by ..\setup.bat existing.
//  - Installed: the exe sits in an install folder (on PATH), and everything
//    it writes goes under %LOCALAPPDATA%\talktoclaude — logs\ and models\.
//
// Paths are UTF-8 std::strings, like the rest of the codebase.
namespace Paths {

// Directory for log files. Created if missing.
std::string logDir();

// The model to load when none is given on the command line. Looks for
// ggml-base.en.bin in the repo layout first, then the LocalAppData models
// folder; if neither has it, downloads it into the latter (142 MB, once)
// with progress on the console. Returns an empty string if that fails.
std::string defaultModel();

}  // namespace Paths
