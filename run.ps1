# One-step test run: builds, runs the game with scripted presses, converts the
# dumped frames to PNG and copies a short summary to the clipboard.
#
#   .\run.ps1 -Press "8:UP","9:A@0.15" -StopAt 15
#   .\run.ps1 -Press "8:UP","9:A@0.15" -StopAt 15 -DumpEvery 240   (also save frames)
#   .\run.ps1 -Press "8:UP","9:A@0.15" -StopAt 15 -XfbEvery 30     (TV frame buffer twice a second)
#   .\run.ps1 -StopAt 7 -TraceAt 6                                  (log frame-pacing calls at 6 s)
#   .\run.ps1 -StopAt 13 -DrawLogAt 12                              (draw calls + GPU state -> drawlog.txt)
#
# Afterwards just paste (Ctrl+V) into the chat. The full log is in run.txt.
param(
    [string[]]$Press = @(),
    [double]$StopAt = 20,
    [int]$DumpEvery = 0,
    [int]$XfbEvery = 0,
    [double]$TraceAt = 0,
    [double]$DrawLogAt = -1,
    [string]$Disc = "C:\nfls2\disc",
    [string]$Map = "C:\nfls2\symbols\main.map"
)
$ErrorActionPreference = "Continue"   # the runner's warnings on stderr aren't errors
Set-Location $PSScriptRoot

# Build tools: a plain PowerShell window doesn't have CMake on its PATH, and the
# "Developer PowerShell for VS" shortcut sets up 32-bit (x86) libraries, which
# can't link the 64-bit game. Unless a 64-bit Visual Studio environment is
# already loaded, load one from the Visual Studio install that has the C++
# build tools (vcvars64.bat), and take over its settings.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -or $env:VSCMD_ARG_TGT_ARCH -ne "x64") {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vcvars = $null
    # Prefer the Visual Studio the build folder was set up with.
    $cache = Join-Path $PSScriptRoot "build\CMakeCache.txt"
    if (Test-Path $cache) {
        $line = Select-String -Path $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.*)\\Common7\\' | Select-Object -First 1
        if (-not $line) { $line = Select-String -Path $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.*)/Common7/' | Select-Object -First 1 }
        if ($line) {
            $bat = Join-Path $line.Matches[0].Groups[1].Value "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $bat) { $vcvars = $bat }
        }
    }
    if (-not $vcvars -and (Test-Path $vswhere)) {
        $installs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        foreach ($vs in $installs) {
            $bat = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $bat) { $vcvars = $bat }
        }
    }
    if ($vcvars) {
        Write-Host "Loading the Visual Studio build tools (64-bit) from $vcvars"
        $vars = cmd /c "set LIB=& set LIBPATH=& set INCLUDE=& call `"$vcvars`" >nul 2>&1 & set"
        foreach ($line in $vars) {
            if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
        }
    }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        Write-Host "CMake not found. Open 'x64 Native Tools Command Prompt for VS 2022' from the Start menu, type powershell, and run this again." -ForegroundColor Red
        exit 1
    }
}

cmake --build build
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed." -ForegroundColor Red; exit 1 }

if (Test-Path frames) { Remove-Item frames\* -ErrorAction SilentlyContinue }

$gameArgs = @($Disc, "--map", $Map, "--quiet", "--debug", "--max-steps", "2000000000",
              "--stop-at", $StopAt, "--dump-every", $DumpEvery, "--xfb-every", $XfbEvery, "--trace-at", $TraceAt)
foreach ($p in $Press) { $gameArgs += @("--press", $p) }
if ($DrawLogAt -ge 0) { $gameArgs += @("--draw-log-at", $DrawLogAt) }

Write-Host "Running until $StopAt s of game time..."
& .\build\nfls2.exe @gameArgs *> run.txt

# Summary: press log, first and last saved frames, and the end-of-run report.
$log = Get-Content run.txt
$summary = @()
$summary += $log | Select-String -Pattern "pad:|file open at|stream:|ax: +[0-9]|cblist:" | ForEach-Object { $_.Line }
$summary += $log | Select-String -Pattern "trace:|draw log" | ForEach-Object { $_.Line }
foreach ($pattern in @("^  frame \d+ saved", "^  tv frame")) {
    $saved = @($log | Select-String -Pattern $pattern | ForEach-Object { $_.Line })
    if ($saved.Count -gt 0) { $summary += $saved[0]; $summary += $saved[-1] }
}
$start = ($log | Select-String -Pattern "^=== stopped" | Select-Object -Last 1).LineNumber
if ($start) { $summary += $log[($start - 1)..($log.Count - 1)] }
else { $summary += $log | Select-Object -Last 60 }
$summary | Set-Content summary.txt

if (($DumpEvery -gt 0 -or $XfbEvery -gt 0) -and (Test-Path frames)) {
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
