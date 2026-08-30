@echo off
REM Everything except installing Visual Studio Build Tools (you're doing that
REM yourself). Run this after the C++ workload finishes installing.
REM
REM Locates the MSVC toolchain itself (via vswhere + vcvarsall.bat) so this
REM works from a plain cmd/PowerShell session — no need to open a special
REM "Developer Command Prompt". Then configures + builds, and fetches a
REM whisper model if missing.

setlocal enabledelayedexpansion

REM Launched by double-clicking from Explorer? Then cmd closes the window the
REM moment we exit, taking all the output with it — pause at the end instead.
set DOUBLECLICKED=
echo %cmdcmdline% | find /i "%~f0" >nul && set DOUBLECLICKED=1

REM build\ gets deleted at the end, so the configure/build output is teed into
REM a timestamped file under logs\ (gitignored) that outlives it.
set LOGDIR=%~dp0logs
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"`) do set STAMP=%%i
set LOG=%LOGDIR%\setup-%STAMP%.log
echo Logging to %LOG%

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo Can't find vswhere.exe — is Visual Studio Build Tools installed?
    goto :fail
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set VSINSTALL=%%i
)

if not defined VSINSTALL (
    echo Couldn't find a VS install with the C++ ^(VC.Tools^) workload.
    echo Make sure "Desktop development with C++" finished installing.
    goto :fail
)

echo Found VS install: %VSINSTALL%

call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if %ERRORLEVEL% neq 0 goto :fail

REM Work from the repo root and pass the generator via the environment: the
REM :tee helper hands its command line to PowerShell, which eats embedded
REM double quotes, so the teed commands have to be quote-free.
cd /d "%~dp0"
set CMAKE_GENERATOR=NMake Makefiles

echo === Configuring ===
call :tee cmake -B build -S .
if %ERRORLEVEL% neq 0 goto :fail

echo === Building ===
call :tee cmake --build build
if %ERRORLEVEL% neq 0 goto :fail

if not exist "%~dp0models" mkdir "%~dp0models"
if not exist "%~dp0models\ggml-base.en.bin" (
    REM Downloaded with curl.exe (shipped with Windows) rather than
    REM whisper.cpp's own download-ggml-model.cmd: that one uses BITS, which
    REM refuses to start on a metered or "no active connection" adapter and
    REM then reports success anyway. It also lives under build\, which we
    REM delete.
    echo === Fetching base.en model ===
    curl.exe -L -f -o "%~dp0models\ggml-base.en.bin" https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin
)
if not exist "%~dp0models\ggml-base.en.bin" (
    echo Model download failed — no models\ggml-base.en.bin.
    echo Grab it manually from https://huggingface.co/ggerganov/whisper.cpp/tree/main
    goto :fail
)

echo === Staging binaries into bin\ ===
if not exist "%~dp0bin" mkdir "%~dp0bin"
copy /y "%~dp0build\talktoclaude.exe" "%~dp0bin\" >nul
if %ERRORLEVEL% neq 0 goto :fail
copy /y "%~dp0build\*.dll" "%~dp0bin\" >nul
if %ERRORLEVEL% neq 0 goto :fail

REM Everything left in build\ is intermediates (objects, CMake cache, the
REM fetched whisper.cpp checkout). Dropping it keeps the tree small; the
REM cost is that re-running setup.bat re-clones and rebuilds whisper.cpp.
REM CMake's own configure log is the one thing in there worth keeping.
if exist "%~dp0build\CMakeFiles\CMakeConfigureLog.yaml" (
    copy /y "%~dp0build\CMakeFiles\CMakeConfigureLog.yaml" "%LOGDIR%\cmake-configure-%STAMP%.yaml" >nul
)

echo === Cleaning build artifacts ===
rmdir /s /q "%~dp0build"

echo.
echo === Done ===
echo Run it with: bin\talktoclaude.exe models\ggml-base.en.bin
echo Build log:   %LOG%
if defined DOUBLECLICKED pause
exit /b 0

:fail
echo.
echo Setup failed — see errors above.
if defined LOG echo Full output: %LOG%
if defined DOUBLECLICKED pause
exit /b 1

REM Runs its arguments as a command, showing output on the console and
REM appending it to %LOG%, and propagates the command's exit code (a plain
REM `| powershell` pipe would report PowerShell's instead).
REM Not Tee-Object: in Windows PowerShell it writes UTF-16 and it would keep
REM stderr lines wrapped as ErrorRecords, complete with PowerShell's own
REM "Au caractere Ligne:1" framing. Flattening each line by hand avoids both.
REM One StreamWriter held open for the whole pipeline, not Add-Content per
REM line: reopening the file thousands of times loses lines to sharing
REM violations whenever something else (an indexer, AV) has it open.
:tee
powershell -NoProfile -Command "$w = [IO.StreamWriter]::new($env:LOG, $true); & cmd /c '%*' 2>&1 | ForEach-Object { $line = $_.ToString(); Write-Host $line; $w.WriteLine($line) }; $w.Close(); exit $LASTEXITCODE"
exit /b %ERRORLEVEL%
