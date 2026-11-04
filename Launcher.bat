@echo off
rem Double-click to open the launcher: set the port up and play, in a window.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0launcher.ps1"
