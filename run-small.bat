@echo off
REM Same as run.bat but with the small.en model - the [DESKTOP] accuracy experiment.
REM Fetches the model on first use; setup.bat only ships base.en. Downloaded
REM via .part + rename so a truncated fetch never looks like a model.
if not exist "%~dp0models" mkdir "%~dp0models"
if not exist "%~dp0models\ggml-small.en.bin" (
    echo === Fetching small.en model ===
    curl.exe -L -f -o "%~dp0models\ggml-small.en.bin.part" https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin
    if errorlevel 1 exit /b 1
    move /y "%~dp0models\ggml-small.en.bin.part" "%~dp0models\ggml-small.en.bin" >nul
)
call "%~dp0run.bat" "%~dp0models\ggml-small.en.bin"
