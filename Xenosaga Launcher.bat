@echo off
rem Xenosaga Episode I: Recompiled - builds the game from your own disc image and starts it.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0launcher\launcher.ps1"
