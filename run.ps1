# One-step test run: builds, runs the game with scripted presses, converts the
# dumped frames to PNG and copies a short summary to the clipboard.
#
#   .\run.ps1 -Press "8:UP","9:A@0.15" -StopAt 15
#   .\run.ps1 -Press "8:UP","9:A@0.15" -StopAt 15 -DumpEvery 240   (also save frames)
#
# Afterwards just paste (Ctrl+V) into the chat. The full log is in run.txt.
param(
    [string[]]$Press = @(),
    [double]$StopAt = 20,
    [int]$DumpEvery = 0,
    [string]$Disc = "C:\nfls2\disc",
    [string]$Map = "C:\nfls2\symbols\main.map"
)
$ErrorActionPreference = "Continue"   # the runner's warnings on stderr aren't errors
Set-Location $PSScriptRoot

cmake --build build
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed." -ForegroundColor Red; exit 1 }

if (Test-Path frames) { Remove-Item frames\* -ErrorAction SilentlyContinue }

$gameArgs = @($Disc, "--map", $Map, "--quiet", "--max-steps", "2000000000",
              "--stop-at", $StopAt, "--dump-every", $DumpEvery)
foreach ($p in $Press) { $gameArgs += @("--press", $p) }

Write-Host "Running until $StopAt s of game time..."
& .\build\nfls2.exe @gameArgs *> run.txt

# Summary: press log, first and last saved frames, and the end-of-run report.
$log = Get-Content run.txt
$summary = @()
$summary += $log | Select-String -Pattern "pad:" | ForEach-Object { $_.Line }
$frames = @($log | Select-String -Pattern "frame \d+ saved" | ForEach-Object { $_.Line })
if ($frames.Count -gt 0) { $summary += $frames[0]; $summary += $frames[-1] }
$start = ($log | Select-String -Pattern "^=== stopped" | Select-Object -Last 1).LineNumber
if ($start) { $summary += $log[($start - 1)..($log.Count - 1)] }
else { $summary += $log | Select-Object -Last 60 }
$summary | Set-Content summary.txt

if ($DumpEvery -gt 0 -and (Test-Path frames)) {
    Add-Type -AssemblyName System.Drawing
    Get-ChildItem frames\*.bmp | ForEach-Object {
        $img = [System.Drawing.Image]::FromFile($_.FullName)
        $img.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item $_.FullName
    }
}

$summary -join "`r`n" | Set-Clipboard
Write-Host "Done. Summary copied to the clipboard: paste it into the chat (Ctrl+V)." -ForegroundColor Green
