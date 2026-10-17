# Builds the port and plays the game in a window at real-time speed.
#
#   .\play.ps1                 (window at 2x, 1280x960)
#   .\play.ps1 -Scale 3        (bigger window)
#
# Keyboard: arrows = stick, X = A, Z = B, C = X, S = Y, D = Z, Q / W = L / R,
#           Enter = START, I J K L = C-stick, T F G H = D-pad
#           Esc = quit, F11 = fullscreen.   A USB/XInput controller also works.
# The log (and the end-of-run report) goes to play.txt.
param(
    [int]$Scale = 2,
    [string[]]$Press = @(),
    [string]$Disc = "C:\nfls2\disc",
    [string]$Map = "C:\nfls2\symbols\main.map"
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot

cmake --build build
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed." -ForegroundColor Red; exit 1 }

$gameArgs = @($Disc, "--map", $Map, "--quiet", "--window", "--scale", $Scale, "--dump-every", "0")
foreach ($p in $Press) { $gameArgs += @("--press", $p) }

Write-Host "Starting the game window... (Esc to quit)"
& .\build\nfls2.exe @gameArgs *> play.txt
Write-Host "Game closed. Log: play.txt" -ForegroundColor Green
