@echo off
rem Double-click to set the port up (or run: setup.bat -Disc "D:\Games\NFL Street 2.iso").
rem Runs setup.ps1 without Windows' script policy getting in the way.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
echo.
pause
