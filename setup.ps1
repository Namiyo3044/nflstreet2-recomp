# One-time setup: everything needed to play, from your own copy of the game.
#
#   .\setup.ps1 -Disc D:\Games\NFL Street 2.iso     (a .iso / .gcm image of the disc)
#   .\setup.ps1 -Disc D:\nfls2-extracted             (or a folder from Dolphin's "Extract Entire Disc")
#   .\setup.ps1                                      (the disc already unpacked in C:\nfls2\disc; otherwise it asks)
#   setup.bat                                        (the same, by double-clicking it)
#
# What it does, each step only if it isn't done yet:
#   1. loads Visual Studio's C++ build tools (they need installing once; see README)
#   2. unpacks the disc image into C:\nfls2\disc and checks main.dol is the right version
#   3. downloads DolRecomp (the recompiler) into C:\nfls2\tools\dolrecomp and builds it
#   4. recompiles the game's code from your main.dol into C:\nfls2\recomp\dolrecomp\generated
#   5. builds the port (the first build takes several minutes), with an icon made from your disc's banner
#   6. puts an "NFL Street 2 Recomp" shortcut on the desktop
# Then play with that shortcut, play.bat or .\play.ps1.
#
#   -Root D:\nfls2     (put all of it somewhere other than C:\nfls2)
#   -Regenerate        (run step 4 again even if its output is there)
#   -NoShortcut        (no desktop shortcut)
param(
    [string]$Disc = "",
    [string]$Root = "C:\nfls2",
    [switch]$Regenerate,
    [switch]$NoShortcut
)
$ErrorActionPreference = "Continue"   # (native tools write progress to stderr; each step checks its result)
Set-Location $PSScriptRoot

# DolRecomp, at the version this port is built and tested against.
$DolRecompCommit = "02e27aca1c408cfcb2a4079a3a5b5ab3ae39d340"
$DolSha1 = "3561e946e9ff785b68692f87d2ba85d81fd2dcf4"   # NFL Street 2, GameCube, USA (GN7E69), rev 0

function Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Fail($text) { Write-Host $text -ForegroundColor Red; exit 1 }

$discDir = Join-Path $Root "disc"
$toolsDir = Join-Path $Root "tools"
$drDir = Join-Path $toolsDir "dolrecomp"
$genParent = Join-Path $Root "recomp\dolrecomp"
$genDir = Join-Path $genParent "generated"
New-Item -ItemType Directory -Force -Path $Root, $toolsDir | Out-Null

# 1. Build tools ----------------------------------------------------------
Step "Build tools"
. (Join-Path $PSScriptRoot "tools\vsenv.ps1")
foreach ($t in @("cmake", "ninja", "clang-cl", "cl")) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        Fail "$t not found: install Visual Studio's 'Desktop development with C++' with 'C++ Clang tools for Windows' (see README)."
    }
}
Write-Host "cmake, ninja, cl and clang-cl found."

# 2. The disc ---------------------------------------------------------------
Step "Game disc"
$extract = Join-Path $toolsDir "nfls2_discextract.exe"
if ($Disc -eq "" -and -not (Test-Path (Join-Path $discDir "sys\main.dol"))) {
    Write-Host "Where is your NFL Street 2 disc? Drag the .iso (or the extracted disc folder) onto this window, then press Enter."
    $Disc = (Read-Host "Disc").Trim().Trim('"')
    if ($Disc -eq "") { Fail "No disc given." }
}
if ($Disc -ne "" -and (Test-Path $Disc -PathType Leaf)) {
    if ($Disc -notmatch '\.(iso|gcm)$') { Fail "$Disc isn't a .iso / .gcm image (convert .rvz / .ciso to .iso in Dolphin: right-click the game, Convert File)." }
    if (-not (Test-Path $extract) -or (Get-Item $extract).LastWriteTime -lt (Get-Item "tools\discextract.c").LastWriteTime) {
        & clang-cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS "tools\discextract.c" "/Fe:$extract" "/Fo:$toolsDir\discextract.obj" | Out-Host
        if ($LASTEXITCODE -ne 0) { Fail "Couldn't build the disc unpacker." }
    }
    Write-Host "Unpacking $Disc into $discDir ..."
    & $extract $Disc $discDir GN7E69
    if ($LASTEXITCODE -ne 0) { Fail "Couldn't unpack the disc image." }
} elseif ($Disc -ne "") {
    if (-not (Test-Path (Join-Path $Disc "sys\main.dol"))) { Fail "$Disc has no sys\main.dol: give a disc image or a folder from Dolphin's 'Extract Entire Disc'." }
    $discDir = $Disc
}
$dol = Join-Path $discDir "sys\main.dol"
if (-not (Test-Path $dol)) { Fail "No game disc found at $discDir. Run again with -Disc and your disc image (e.g. -Disc D:\nfls2.iso)." }
$hash = (Get-FileHash -Algorithm SHA1 $dol).Hash.ToLower()
if ($hash -ne $DolSha1) { Fail "This main.dol isn't NFL Street 2 for GameCube, USA, revision 0 (SHA-1 $hash). The port only works with that version." }
Write-Host "main.dol is NFL Street 2 (GN7E69 rev 0)."

# The program's icon, from the disc's own banner (the picture Dolphin's game list shows).
$icon = Join-Path $Root "icon.ico"
$bnr = Join-Path $discDir "files\opening.bnr"
if (-not (Test-Path $icon) -and (Test-Path $bnr)) {
    $makeicon = Join-Path $toolsDir "nfls2_makeicon.exe"
    & clang-cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS "tools\makeicon.c" "/Fe:$makeicon" "/Fo:$toolsDir\makeicon.obj" | Out-Host
    if ($LASTEXITCODE -eq 0) { & $makeicon $bnr $icon | Out-Host }
    if (-not (Test-Path $icon)) { Write-Host "(couldn't make the icon from the disc's banner; carrying on without it)" }
}

# 3. DolRecomp --------------------------------------------------------------
Step "DolRecomp"
if (-not (Test-Path (Join-Path $drDir "src\cpu\cpu.c"))) {
    $zip = Join-Path $toolsDir "dolrecomp.zip"
    Write-Host "Downloading DolRecomp ($($DolRecompCommit.Substring(0, 7)))..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try {
        Invoke-WebRequest -UseBasicParsing "https://github.com/expansionpak/dolrecomp/archive/$DolRecompCommit.zip" -OutFile $zip
    } catch { Fail "Couldn't download DolRecomp: $($_.Exception.Message)" }
    $tmp = Join-Path $toolsDir "dolrecomp-unpack"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $zip -DestinationPath $tmp
    Move-Item (Get-ChildItem $tmp | Select-Object -First 1).FullName $drDir
    Remove-Item -Recurse -Force $tmp, $zip
} else {
    Write-Host "Using the DolRecomp already in $drDir."
}
$drExe = Join-Path $drDir "build\dolrecomp.exe"
$drRelease = Join-Path $drDir "build\Release\dolrecomp.exe"   # (a Visual Studio-generator build of your own)
if (-not (Test-Path $drExe) -and (Test-Path $drRelease)) { $drExe = $drRelease }
if (-not (Test-Path $drExe)) {
    Write-Host "Building DolRecomp..."
    & cmake -S $drDir -B (Join-Path $drDir "build") -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl | Out-Host
    & cmake --build (Join-Path $drDir "build") --target dolrecomp | Out-Host
    if (-not (Test-Path $drExe)) { Fail "Couldn't build DolRecomp." }
}

# 4. Recompile the game's code ------------------------------------------------
Step "Recompiling the game's code"
if ($Regenerate -or -not (Test-Path (Join-Path $genDir "generated.h"))) {
    New-Item -ItemType Directory -Force -Path $genParent | Out-Null
    $map = Join-Path $PSScriptRoot "data\gn7e69-functions.map"
    & $drExe --gamecube --map $map $dol $genParent | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $genDir "generated.h"))) { Fail "DolRecomp failed." }
} else {
    Write-Host "Already done ($genDir)."
}

# 5. Build the port -----------------------------------------------------------
Step "Building the port"
$iconArg = "-DNFLS2_ICON=$($icon -replace '\\','/')"
if (-not (Test-Path "build\CMakeCache.txt")) {
    & cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release `
        "-DNFLS2_DOLRECOMP_DIR=$($drDir -replace '\\','/')" "-DNFLS2_GENERATED_DIR=$($genDir -replace '\\','/')" $iconArg | Out-Host
} else {
    & cmake -S . -B build $iconArg | Out-Host   # (picks up the icon if it's new)
}
if ($LASTEXITCODE -ne 0) { Fail "CMake couldn't set up the build." }
& cmake --build build | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "The build failed." }

# 6. A desktop shortcut ---------------------------------------------------------
$playArgs = if ($discDir -ne "C:\nfls2\disc") { "-Disc `"$discDir`"" } else { "" }
if (-not $NoShortcut) {
    $lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "NFL Street 2 Recomp.lnk"
    if (-not (Test-Path $lnk)) {
        $sh = New-Object -ComObject WScript.Shell
        $sc = $sh.CreateShortcut($lnk)
        $sc.TargetPath = Join-Path $PSScriptRoot "play.bat"
        $sc.Arguments = $playArgs
        $sc.WorkingDirectory = $PSScriptRoot
        $sc.WindowStyle = 7                # (the launcher's console window minimized)
        $sc.Description = "NFL Street 2 Recomp"
        if (Test-Path $icon) { $sc.IconLocation = "$icon,0" }
        $sc.Save()
        Write-Host "Made a desktop shortcut: NFL Street 2 Recomp"
    }
}

Write-Host ""
Write-Host "All set. Play with the desktop shortcut, play.bat or:  .\play.ps1 $playArgs" -ForegroundColor Green
