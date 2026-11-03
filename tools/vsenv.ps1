# Loads a 64-bit Visual Studio C++ build environment into this PowerShell
# session (dot-sourced by play.ps1, run.ps1 and setup.ps1).
#
# Build tools: a plain PowerShell window doesn't have CMake on its PATH, and the
# "Developer PowerShell for VS" shortcut sets up 32-bit (x86) libraries, which
# can't link the 64-bit game. Unless a 64-bit Visual Studio environment is
# already loaded, load one from the Visual Studio install that has the C++
# build tools (vcvars64.bat), and take over its settings.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -or $env:VSCMD_ARG_TGT_ARCH -ne "x64") {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vcvars = $null
    # Prefer the Visual Studio the build folder was set up with.
    $cache = Join-Path (Split-Path $PSScriptRoot) "build\CMakeCache.txt"
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
        Write-Host "Visual Studio's C++ build tools weren't found (no CMake)." -ForegroundColor Red
        Write-Host "Install 'Build Tools for Visual Studio 2022' (free, visualstudio.microsoft.com/downloads) with the"
        Write-Host "'Desktop development with C++' workload and its 'C++ Clang tools for Windows' option, then run this again."
        exit 1
    }
}
# The game is built with clang-cl (Visual Studio's "C++ Clang tools for Windows").
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
    $llvm = $null
    if ($env:VCINSTALLDIR) { $llvm = Join-Path $env:VCINSTALLDIR "Tools\Llvm\x64\bin" }
    if ($llvm -and (Test-Path (Join-Path $llvm "clang-cl.exe"))) {
        $env:PATH = "$llvm;$env:PATH"
    } else {
        Write-Host "clang-cl not found. In the Visual Studio Installer, modify your Visual Studio / Build Tools and tick" -ForegroundColor Red
        Write-Host "'C++ Clang tools for Windows' (under 'Desktop development with C++'), then run this again."
        exit 1
    }
}
