@echo off
REM Everything except installing Visual Studio Build Tools (you're doing that
REM yourself). Run this after the C++ workload finishes installing.
REM
REM Locates the MSVC toolchain itself (via vswhere + vcvarsall.bat) so this
REM works from a plain cmd session — no need to open a special "Developer
REM Command Prompt". Then configures + builds, and fetches a whisper model
REM if missing.
REM
REM Incremental by default: build\ is kept, so a source edit rebuilds in
REM seconds instead of re-cloning and recompiling whisper.cpp (minutes).
REM Pass `setup.bat clean` to delete build\ first and start from scratch.
REM
REM Pure cmd on purpose. cmd has no tee, so the noisy steps write their full
REM output to the log rather than the console; on failure the error lines are
REM pulled back out of it. Progress is reported by the === milestones ===.

setlocal enabledelayedexpansion

set CLEAN=
if /i "%~1"=="clean" set CLEAN=1

REM Launched by double-clicking from Explorer? Then cmd closes the window the
REM moment we exit, taking all the output with it — pause at the end instead.
set DOUBLECLICKED=
echo %cmdcmdline% | find /i "%~f0" >nul && set DOUBLECLICKED=1

REM Configure/build output goes to a timestamped file under logs\, so a
REM `clean` run that throws build\ away still leaves the record behind.
set LOGDIR=%~dp0logs
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
call :stamp
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

REM Deliberately not redirected into %LOG%: vcvarsall leaves something holding
REM the redirected file open after it returns, and the next append then fails
REM with a sharing violation. Its banner is five lines, so console is fine.
REM Build Tools 18's VsDevCmd.bat calls vswhere.exe by bare name and prints
REM "not recognized" when it isn't on PATH (harmless; it carries on). Put the
REM installer directory first so the console stays clean.
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if %ERRORLEVEL% neq 0 goto :fail

if defined CLEAN (
    echo === Cleaning build\ ===
    if exist "%~dp0build" rmdir /s /q "%~dp0build"
)

echo === Configuring ===
cmake -B build -S "%~dp0." -G "NMake Makefiles" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 goto :fail
REM A redirect that can't open the log leaves ERRORLEVEL at 0 while the
REM command never runs, so check for what the step was supposed to produce.
if not exist "%~dp0build\CMakeCache.txt" (
    echo Configure produced no build\CMakeCache.txt.
    goto :fail
)

echo === Building ^(minutes from clean, seconds otherwise; watch %LOG% for detail^) ===
cmake --build build >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 goto :fail
if not exist "%~dp0build\talktoclaude.exe" (
    echo Build produced no build\talktoclaude.exe.
    goto :fail
)

if not exist "%~dp0models" mkdir "%~dp0models"
if not exist "%~dp0models\ggml-base.en.bin" (
    REM Downloaded with curl.exe (shipped with Windows) rather than
    REM whisper.cpp's own download-ggml-model.cmd: that one uses BITS, which
    REM refuses to start on a metered or "no active connection" adapter and
    REM then reports success anyway. It also lives under build\, which we
    REM delete. Not redirected — curl's progress bar is the one thing worth
    REM watching live, since it's 142 MB.
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

REM build\ stays put: it holds the fetched whisper.cpp checkout and the
REM object files, which is exactly what makes the next run fast. It is
REM gitignored, so the only cost is disk. `setup.bat clean` wipes it.

echo.
echo === Done ===
echo Run it with: bin\talktoclaude.exe models\ggml-base.en.bin
echo Build log:   %LOG%
if not defined CLEAN echo Re-run this after editing a source file; pass `clean` to rebuild from scratch.
if defined DOUBLECLICKED pause
exit /b 0

:fail
echo.
echo Setup failed. Error lines from the log:
echo.
if exist "%LOG%" findstr /i /c:"error" /c:"fatal" "%LOG%"
echo.
if defined LOG echo Full output: %LOG%
if defined DOUBLECLICKED pause
exit /b 1

REM Sets %STAMP% to a sortable YYYYMMDD-HHMMSS. %DATE%/%TIME% are the only
REM clock cmd has (wmic is gone from Windows 11 26200), and both are
REM locale-shaped: here they read 30/08/2026 and 14:16:49,37. Day-name
REM prefixes are dropped, and a leading 4-digit token is taken as a
REM year-first locale; otherwise day-before-month is assumed, which is what
REM this machine uses.
:stamp
set _d=%DATE%
set _t=%TIME%
for /f "tokens=1-4 delims=/-. " %%a in ("%_d%") do (
    set _p1=%%a& set _p2=%%b& set _p3=%%c& set _p4=%%d
)
if defined _p4 (
    set _p1=!_p2!& set _p2=!_p3!& set _p3=!_p4!
)
if "!_p1:~3,1!"=="" (
    set _yyyy=!_p3!& set _mm=!_p2!& set _dd=!_p1!
) else (
    set _yyyy=!_p1!& set _mm=!_p2!& set _dd=!_p3!
)
for /f "tokens=1-3 delims=:,." %%a in ("%_t%") do (
    set _hh=0%%a& set _mi=%%b& set _ss=%%c
)
set _hh=!_hh:~-2!
set STAMP=!_yyyy!!_mm!!_dd!-!_hh!!_mi!!_ss!
exit /b 0
