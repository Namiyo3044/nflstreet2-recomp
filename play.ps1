# Builds the port and plays the game in a window at real-time speed.
#
#   .\play.ps1                 (with the settings from the video menu, F1 in the game window; saved in C:\nfls2\settings.ini)
#   The switches below override those settings for one run:
#   .\play.ps1 -Scale 3        (window size, N x 640x480)
#   .\play.ps1 -Res 2          (graphics card's internal resolution, N x 640x528)
#   .\play.ps1 -Debug          (extra diagnostics: call counts, file/stream logs, profile at exit; a little slower)
#   .\play.ps1 -Software       (the old CPU renderer instead of the graphics card)
#   .\play.ps1 -Textures D:\pack (custom textures in Dolphin's format, .png or .dds; default C:\nfls2\textures, used if it exists)
#   .\play.ps1 -NoTextures     (the game's own textures only)
#   .\play.ps1 -Replace D:\dir (files there are read instead of the disc's, e.g. moviedat.dat; default C:\nfls2\replace, used if it exists)
#   .\play.ps1 -DumpAudio     (record the sound to C:\nfls2\dump\audio.wav)
#   .\play.ps1 -UisLog        (log each kind of call the menu scripts make, "uis: call" lines)
#   .\play.ps1 -UisTrace      (log every call the menu scripts make, in order, "uist:" lines)
#   .\play.ps1 -DumpTextures   (save every texture the game uses to C:\nfls2\dump\textures, with Dolphin's names)
#   .\play.ps1 -NoText         (game text exactly as on the disc; ignores text-replacements.txt)
#   .\play.ps1 -NoUiColors    (the game's own menu shape colours; ignores ui-colors.txt)
#   .\play.ps1 -NoMemCard      (no memory card; by default C:\nfls2\saves\MemoryCardA.raw, made if missing)
#   .\play.ps1 -ShowTHX        (keep the THX intro; it is skipped by default)
#   .\play.ps1 -Aspect 16:9    (screen shape for this run: 4:3 or 16:9)
#   .\play.ps1 -Record         (netplay groundwork: record every controller input to C:\nfls2\netsync\input.rec, with
#                               a sync log of game memory; the memory card isn't written to)
#   .\play.ps1 -Replay         (play that recording back, write a second sync log and compare the two)
#   .\play.ps1 -Online         (show the main menu's Online, hidden for now: host or join a game from there)
#   .\play.ps1 -NetHost        (netplay: wait for another player to join; you are player 1. -NetPort N, default 7120)
#   .\play.ps1 -Join 1.2.3.4   (netplay: join a host at that address; you are player 2. Log in play-join.txt)
#                               -NetDelay N: fields of input delay (default 3; more for a distant connection).
#                               Both players need the same settings that change the game (screen shape, rain,
#                               Skip Intro Videos, text-replacements.txt, ui-colors.txt); the host's memory card is used.
#
# Keyboard (the defaults; change them in the F1 menu under Controls): arrows = stick, X = A, Z = B, C = X, S = Y, D = Z, Q / W = L / R,
#           Enter = START, I J K L = C-stick, T F G H = D-pad
#           Esc = quit, F1 (or the controller's Back / View button) = video menu, F5 = custom textures on / off, Tab (hold) = fast-forward, F9 = screen shape (4:3 / 16:9), F10 = screenshot (shot_NNN.bmp), F11 = fullscreen,
#           F12 = draw log of two frames (drawlog.txt, drawlog-short.txt), Shift+F8 = memory dump (memdump.bin, for debugging).
#           A USB/XInput controller also works.
# The log (and the end-of-run report) goes to play.txt.
param(
    [int]$Scale = 2,
    [string]$Aspect = "auto",
    [string]$Settings = "C:\nfls2\settings.ini",
    [switch]$GPU,                      # the default now; kept so older commands still work
    [switch]$Software,
    [switch]$Debug,
    [int]$Res = 4,
    [switch]$NoIdleSkip,
    [switch]$ShowTHX,
    [string]$MemCard = "C:\nfls2\saves\MemoryCardA.raw",
    [switch]$NoMemCard,
    [switch]$NoText,
    [switch]$NoUiColors,
    [string]$Textures = "C:\nfls2\textures",
    [string]$Replace = "C:\nfls2\replace",
    [switch]$NoTextures,
    [switch]$DumpTextures,
    [switch]$DumpAudio,
    [switch]$UisLog,
    [switch]$UisTrace,
    [switch]$LodLog,
    [switch]$CardTrace,
    [switch]$Record,
    [switch]$Replay,
    [switch]$Online,
    [switch]$NetHost,
    [string]$Join = "",
    [int]$NetPort = 7120,
    [int]$NetDelay = 3,
    [string[]]$Press = @(),
    [string]$Disc = "C:\nfls2\disc",
    [string]$Map = "C:\nfls2\symbols\main.map"
)
$ErrorActionPreference = "Continue"
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

$second = [bool](Get-Process nfls2 -ErrorAction SilentlyContinue)
if ($second) {
    # (a second copy, e.g. both netplay players on one PC: the running game can't be rebuilt)
    Write-Host "The game is already running: using the build as it is (this copy logs to play-2.txt)."
} else {
    cmake --build build
    if ($LASTEXITCODE -ne 0) { Write-Host "Build failed." -ForegroundColor Red; exit 1 }
}

New-Item -ItemType Directory -Force -Path (Split-Path $Settings) | Out-Null
$gameArgs = @($Disc, "--map", $Map, "--quiet", "--window", "--dump-every", "0", "--settings", $Settings)
if ($PSBoundParameters.ContainsKey("Scale")) { $gameArgs += @("--scale", $Scale) }
if ($PSBoundParameters.ContainsKey("Aspect")) { $gameArgs += @("--aspect", $Aspect) }
foreach ($p in $Press) { $gameArgs += @("--press", $p) }
if ($NoIdleSkip) { $gameArgs += "--no-idle-skip" }
if (-not $ShowTHX) { $gameArgs += "--skip-thx" }
if ($NoText) { $gameArgs += @("--no-text") }
if ($NoUiColors) { $gameArgs += @("--no-ui-colors") }
if ($Software) { $gameArgs += @("--software") }
elseif ($PSBoundParameters.ContainsKey("Res")) { $gameArgs += @("--gpu", "--res", $Res) }
elseif ($GPU) { $gameArgs += @("--gpu") }
if ($Debug) { $gameArgs += @("--debug") }
if ($CardTrace) { $gameArgs += @("--cardtrace") }
if ($UisLog) { $gameArgs += @("--uis-log") }
if ($UisTrace) { $gameArgs += @("--uis-trace") }
if ($LodLog) { $gameArgs += @("--lod-log") }
if (-not $NoTextures -and (Test-Path $Textures)) { $gameArgs += @("--textures", $Textures) }
if (Test-Path $Replace) { $gameArgs += @("--replace", $Replace) }
if ($DumpAudio) {
    New-Item -ItemType Directory -Force -Path "C:\nfls2\dump" | Out-Null
    $gameArgs += @("--dump-audio", "C:\nfls2\dump\audio.wav")
}
if ($DumpTextures) {
    New-Item -ItemType Directory -Force -Path "C:\nfls2\dump\textures" | Out-Null
    $gameArgs += @("--dump-textures", "C:\nfls2\dump\textures")
}
if (-not $NoMemCard) {
    New-Item -ItemType Directory -Force -Path (Split-Path $MemCard) | Out-Null
    $gameArgs += @("--memcard", $MemCard)
}

$netDir = "C:\nfls2\netsync"
if ($Record -or $Replay) {
    New-Item -ItemType Directory -Force -Path $netDir | Out-Null
    $gameArgs += @("--memcard-readonly")
    if ($Replay) {
        $gameArgs += @("--play-input", "$netDir\input.rec", "--sync-log", "$netDir\sync-replay.txt")
    } else {
        $gameArgs += @("--record-input", "$netDir\input.rec", "--sync-log", "$netDir\sync-record.txt")
    }
}

$logFile = if ($second) { "play-2.txt" } else { "play.txt" }
if ($second -and -not $NoMemCard) {
    $gameArgs += @("--memcard-readonly")   # (the first copy has the card file; this one doesn't save to it)
}
if ($Online) { $gameArgs += @("--online") }
if ($NetHost) {
    $gameArgs += @("--net-host", $NetPort, "--net-delay", $NetDelay)
    Write-Host "Netplay: waiting for the other player on port $NetPort..."
} elseif ($Join -ne "") {
    $addr = if ($Join.Contains(":")) { $Join } else { "${Join}:$NetPort" }
    $gameArgs += @("--net-join", $addr, "--net-delay", $NetDelay)
    $logFile = "play-join.txt"
    Write-Host "Netplay: joining $addr..."
}

Write-Host "Starting the game window... (Esc to quit)"
& .\build\nfls2.exe @gameArgs *> $logFile
# Copy the end-of-run report (where the game stopped and why) to the clipboard.
$log = Get-Content $logFile
$start = ($log | Select-String -Pattern "^=== stopped" | Select-Object -Last 1).LineNumber
if ($start) { $report = $log[($start - 1)..($log.Count - 1)] } else { $report = $log | Select-Object -Last 80 }
# Slow stretches (the window logs a perf line each second the game runs below full speed).
$perf = @($log | Select-String -Pattern "^perf:|^audio:|^hitch:" | ForEach-Object { $_.Line })
$gpuLines = @($log | Select-String -Pattern "^gpu:|^custom textures:" | ForEach-Object { $_.Line })
$startup = @($log | Select-String -Pattern "^netplay:|^netsync:|^startup:|^disc: replacement|^movie:|^window: player|^window: controller|^window: wrote memdump|^uis:|^lod:|^rain:|^credits:|^ui colors:|^window: game.s screen option" | ForEach-Object { $_.Line })
$cbl = @($log | Select-String -Pattern "cblist:" | ForEach-Object { $_.Line })
$txt = @($log | Select-String -Pattern "^text:|^textcall:" | ForEach-Object { $_.Line })
$ct = @($log | Select-String -Pattern "^cardtrace:" | ForEach-Object { $_.Line })
if ($ct.Count -gt 0) { $report = @("card trace ($($ct.Count) lines):") + ($ct | Select-Object -First 1500) + $report }
$rs = @($log | Select-String -Pattern "^ramsearch:" | ForEach-Object { $_.Line })
if ($rs.Count -gt 0) { $report = @("memory search ($($rs.Count) lines):") + ($rs | Select-Object -Last 150) + $report }
if ($txt.Count -gt 0) { $report = @("text search ($($txt.Count) lines):") + ($txt | Select-Object -Last 500) + $report }
if ($cbl.Count -gt 0) { $report = @("sound callback list ($($cbl.Count) lines, last 40):") + ($cbl | Select-Object -Last 40) + $report }
if ($perf.Count -gt 0) { $report = @("slow seconds and hitches: $($perf.Count) lines (last 40 shown)") + ($perf | Select-Object -Last 40) + $report }
if ($startup.Count -gt 0) { $report = $startup + $report }
if ($gpuLines.Count -gt 0) { $report = ($gpuLines | Select-Object -First 30) + $report }
$report -join "`r`n" | Set-Clipboard
Write-Host "Game closed. Report copied to the clipboard (full log: $logFile)" -ForegroundColor Green

if ($Replay -and (Test-Path "$netDir\sync-record.txt") -and (Test-Path "$netDir\sync-replay.txt")) {
    & "$PSScriptRoot\tools\synccompare.ps1" "$netDir\sync-record.txt" "$netDir\sync-replay.txt"
}
