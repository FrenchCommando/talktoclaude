@echo off
REM Same as run.bat but with the large-v3-turbo model - accuracy over latency.
REM Multilingual; transcriber pins output to English. Fetches on first use;
REM .part + rename so a truncated fetch never looks like a model.
if not exist "%~dp0models" mkdir "%~dp0models"
if not exist "%~dp0models\ggml-large-v3-turbo.bin" (
    echo === Fetching large-v3-turbo model ===
    curl.exe -L -f -o "%~dp0models\ggml-large-v3-turbo.bin.part" https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin
    if errorlevel 1 exit /b 1
    move /y "%~dp0models\ggml-large-v3-turbo.bin.part" "%~dp0models\ggml-large-v3-turbo.bin" >nul
)
call "%~dp0run.bat" "%~dp0models\ggml-large-v3-turbo.bin"
