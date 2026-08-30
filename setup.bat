@echo off
REM Everything except installing Visual Studio Build Tools (you're doing that
REM yourself). Run this after the C++ workload finishes installing.
REM
REM Locates the MSVC toolchain itself (via vswhere + vcvarsall.bat) so this
REM works from a plain cmd/PowerShell session — no need to open a special
REM "Developer Command Prompt". Then configures + builds, and fetches a
REM whisper model if missing.

setlocal enabledelayedexpansion

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo Can't find vswhere.exe — is Visual Studio Build Tools installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set VSINSTALL=%%i
)

if not defined VSINSTALL (
    echo Couldn't find a VS install with the C++ ^(VC.Tools^) workload.
    echo Make sure "Desktop development with C++" finished installing.
    exit /b 1
)

echo Found VS install: %VSINSTALL%

call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if %ERRORLEVEL% neq 0 goto :fail

echo === Configuring ===
cmake -B build -S "%~dp0" -G "NMake Makefiles"
if %ERRORLEVEL% neq 0 goto :fail

echo === Building ===
cmake --build build
if %ERRORLEVEL% neq 0 goto :fail

if not exist "%~dp0models" mkdir "%~dp0models"
if not exist "%~dp0models\ggml-base.en.bin" (
    echo === Fetching base.en model ===
    call "%~dp0third_party\whisper.cpp\models\download-ggml-model.cmd" base.en "%~dp0models"
)

echo.
echo === Done ===
echo Run it with: build\talktoclaude.exe models\ggml-base.en.bin
exit /b 0

:fail
echo.
echo Build failed — see errors above.
exit /b 1
