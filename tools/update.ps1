# Updates the port's own files to a newer version (the launcher's Update
# button runs it, then runs setup.ps1 for anything that version needs redone).
# Only the port folder changes: the install folder (disc, recompiled code,
# saves, settings, textures) is left alone.
#
#   tools\update.ps1 -Tag v1.1 -ZipUrl <the release's source zip>
#
# A copy made with git is updated with git (git pull); any other copy gets the
# release's source zip unpacked over it.
param(
    [string]$Tag = "",
    [string]$ZipUrl = ""
)
$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"
$port = Split-Path $PSScriptRoot
Set-Location $port

if ((Test-Path (Join-Path $port ".git")) -and (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "== Updating with git"
    & git pull --ff-only 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "git couldn't update this copy (it may have changes of its own). Update it yourself with git pull."
        exit 1
    }
    exit 0
}

if ($ZipUrl -eq "") { Write-Host "No download given."; exit 1 }
Write-Host "== Downloading $Tag"
$tmp = Join-Path $env:TEMP "nfls2-update"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$zip = Join-Path $tmp "update.zip"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
try {
    Invoke-WebRequest -UseBasicParsing -Headers @{ "User-Agent" = "nfls2-recomp-launcher" } $ZipUrl -OutFile $zip
} catch { Write-Host "Couldn't download the update: $($_.Exception.Message)"; exit 1 }
Expand-Archive $zip -DestinationPath (Join-Path $tmp "files")
$src = Get-ChildItem (Join-Path $tmp "files") -Directory | Select-Object -First 1
if (-not $src -or -not (Test-Path (Join-Path $src.FullName "setup.ps1"))) { Write-Host "The download isn't the port's source."; exit 1 }

Write-Host "== Installing $Tag"
# The folders that hold only the port's own files are replaced whole, so files
# a newer version no longer has don't linger; everything else is copied over.
foreach ($d in @("src", "tools", "data")) {
    $p = Join-Path $port $d
    if ((Test-Path $p) -and (Test-Path (Join-Path $src.FullName $d))) { Remove-Item -Recurse -Force $p }
}
& robocopy $src.FullName $port /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Host "Couldn't copy the new files in (robocopy error $LASTEXITCODE)."; exit 1 }
Remove-Item -Recurse -Force $tmp
Write-Host "Updated to $Tag."
exit 0
