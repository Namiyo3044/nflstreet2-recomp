@echo off
rem Double-click to play (or run: play.bat -Scale 3, any of play.ps1's switches).
rem Runs play.ps1 without Windows' script policy getting in the way.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0play.ps1" %*
if errorlevel 1 pause
