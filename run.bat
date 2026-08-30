@echo off
REM Optional first argument: a model path, to try another one without editing
REM this file. The default is the model to change when you settle on one.
set MODEL=%~1
if not defined MODEL set MODEL=%~dp0models\ggml-base.en.bin
"%~dp0bin\talktoclaude.exe" "%MODEL%"
