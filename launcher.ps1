# The launcher: a window for setting the port up and playing, so nobody has to
# type commands. It runs setup.ps1 and play.ps1 behind the scenes and shows
# their progress. Started by Launcher.bat (double-click it).
#
#   1. Build tools: installs Visual Studio's free C++ build tools (with winget) if they're missing
#   2. Game disc: pick your .iso / .gcm / .nkit.iso
#   3. Set Up: unpacks the disc, recompiles the game's code and builds the port (setup.ps1)
#   4. Play
#
# The choices (disc, install folder) are remembered in %LOCALAPPDATA%\nfls2-recomp\launcher.ini.
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
try {
    Add-Type -Namespace Nfls2 -Name Dpi -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
    [Nfls2.Dpi]::SetProcessDPIAware() | Out-Null
} catch { }
[System.Windows.Forms.Application]::EnableVisualStyles()

$Ok = [string][char]0x2713
$No = [string][char]0x2717

# Remembered choices --------------------------------------------------------------
$iniDir = Join-Path $env:LOCALAPPDATA "nfls2-recomp"
$ini = Join-Path $iniDir "launcher.ini"
$cfg = @{ disc = ""; ps2 = ""; root = "C:\nfls2"; shortcut = "1" }
if (Test-Path $ini) {
    foreach ($line in Get-Content $ini) { if ($line -match '^(\w+)=(.*)$') { $cfg[$Matches[1]] = $Matches[2] } }
}
function Save-Config {
    New-Item -ItemType Directory -Force -Path $iniDir | Out-Null
    $cfg.disc = $discBox.Text.Trim().Trim('"')
    $cfg.ps2 = $ps2Box.Text.Trim().Trim('"')
    $cfg.root = $rootBox.Text.Trim().Trim('"')
    $cfg.shortcut = if ($shortcutBox.Checked) { "1" } else { "0" }
    @("disc=$($cfg.disc)", "ps2=$($cfg.ps2)", "root=$($cfg.root)", "shortcut=$($cfg.shortcut)") | Set-Content -Path $ini -Encoding ASCII
}

# Checks --------------------------------------------------------------------------
function Test-BuildTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    $vc = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vc) { return $false }
    $clang = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
    if ($clang) { return $true }
    if (Get-Command clang-cl -ErrorAction SilentlyContinue) { return $true }
    return (Test-Path (Join-Path $env:ProgramFiles "LLVM\bin\clang-cl.exe"))
}
function Get-Root { $r = $rootBox.Text.Trim().Trim('"'); if ($r -eq "") { "C:\nfls2" } else { $r } }
function Get-DiscFolder {
    # The unpacked disc: an extracted folder given as the disc, or <root>\disc
    $d = $discBox.Text.Trim().Trim('"')
    if ($d -ne "" -and (Test-Path $d -PathType Container)) { return $d }
    return (Join-Path (Get-Root) "disc")
}
function Test-Disc {
    $d = $discBox.Text.Trim().Trim('"')
    if (Test-Path (Join-Path (Get-DiscFolder) "sys\main.dol")) { return $true }
    return ($d -ne "" -and (Test-Path $d -PathType Leaf))
}
function Test-PS2Extras {
    $r = Join-Path (Get-Root) "replace"
    (Test-Path (Join-Path $r "moviedat_1.mpg")) -and (Test-Path (Join-Path $r "previews\nfs_underground_2.mpg"))
}
function Test-Built {
    (Test-Path (Join-Path $PSScriptRoot "build\nfls2.exe")) -and
    (Test-Path (Join-Path (Get-Root) "recomp\dolrecomp\generated\generated.h")) -and
    (Test-Path (Join-Path (Get-DiscFolder) "sys\main.dol"))
}

# The window ----------------------------------------------------------------------
$form = New-Object System.Windows.Forms.Form
$form.Text = "NFL Street 2 Recomp"
$form.AutoScaleDimensions = New-Object System.Drawing.SizeF(96, 96)
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
$form.ClientSize = New-Object System.Drawing.Size(640, 642)
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedSingle
$form.MaximizeBox = $false
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$form.Font = New-Object System.Drawing.Font("Segoe UI", 9)
$icon = Join-Path $(if ($cfg.root) { $cfg.root } else { "C:\nfls2" }) "icon.ico"
if (Test-Path $icon) { try { $form.Icon = New-Object System.Drawing.Icon($icon) } catch { } }

function Add-Label($text, $x, $y, $w, $h, $font) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.Location = New-Object System.Drawing.Point($x, $y)
    $l.Size = New-Object System.Drawing.Size($w, $h)
    if ($font) { $l.Font = $font }
    $form.Controls.Add($l)
    return $l
}
function Add-Button($text, $x, $y, $w) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text
    $b.Location = New-Object System.Drawing.Point($x, $y)
    $b.Size = New-Object System.Drawing.Size($w, 28)
    $form.Controls.Add($b)
    return $b
}

$bold = New-Object System.Drawing.Font("Segoe UI", 9, [System.Drawing.FontStyle]::Bold)
Add-Label "NFL Street 2 Recomp" 16 12 400 30 (New-Object System.Drawing.Font("Segoe UI", 15, [System.Drawing.FontStyle]::Bold)) | Out-Null
Add-Label "A native PC port, built on your PC from your own GameCube disc (USA)." 18 44 600 20 $null | Out-Null

# 1. Build tools
Add-Label "1. Build tools" 16 78 200 20 $bold | Out-Null
$toolsStatus = Add-Label "" 34 100 440 20 $null
$toolsButton = Add-Button "Install Build Tools" 488 95 136

# 2. Disc
Add-Label "2. Your NFL Street 2 disc (.iso, .gcm, .nkit.iso or an extracted folder)" 16 134 600 20 $bold | Out-Null
$discBox = New-Object System.Windows.Forms.TextBox
$discBox.Location = New-Object System.Drawing.Point(34, 158)
$discBox.Size = New-Object System.Drawing.Size(446, 24)
$discBox.Text = $cfg.disc
$form.Controls.Add($discBox)
$discButton = Add-Button "Browse..." 488 156 136
$discStatus = Add-Label "" 34 186 590 20 $null

# Install folder
Add-Label "Install folder (the unpacked disc, recompiled code, saves and settings)" 16 214 600 20 $bold | Out-Null
$rootBox = New-Object System.Windows.Forms.TextBox
$rootBox.Location = New-Object System.Drawing.Point(34, 238)
$rootBox.Size = New-Object System.Drawing.Size(446, 24)
$rootBox.Text = $cfg.root
$form.Controls.Add($rootBox)
$rootButton = Add-Button "Change..." 488 236 136
$shortcutBox = New-Object System.Windows.Forms.CheckBox
$shortcutBox.Text = "Put a shortcut on the desktop"
$shortcutBox.Location = New-Object System.Drawing.Point(34, 266)
$shortcutBox.Size = New-Object System.Drawing.Size(300, 22)
$shortcutBox.Checked = $cfg.shortcut -ne "0"
$form.Controls.Add($shortcutBox)
$texButton = Add-Button "Textures Folder" 488 264 136

# Optional: the PS2 disc
Add-Label "Optional: your PS2 disc (.iso), for the Previews trailers and sharper intro movies" 16 298 608 20 $bold | Out-Null
$ps2Box = New-Object System.Windows.Forms.TextBox
$ps2Box.Location = New-Object System.Drawing.Point(34, 322)
$ps2Box.Size = New-Object System.Drawing.Size(446, 24)
$ps2Box.Text = $cfg.ps2
$form.Controls.Add($ps2Box)
$ps2Button = Add-Button "Browse..." 488 320 136
$ps2Status = Add-Label "" 34 350 590 20 $null

# 3. Set up / 4. Play
Add-Label "3. Set up, then play" 16 380 300 20 $bold | Out-Null
$setupButton = Add-Button "Set Up" 34 404 136
$playButton = Add-Button "Play" 180 404 136
$playButton.Font = $bold
$step = Add-Label "" 330 410 294 20 $null
$progress = New-Object System.Windows.Forms.ProgressBar
$progress.Location = New-Object System.Drawing.Point(34, 442)
$progress.Size = New-Object System.Drawing.Size(590, 10)
$progress.Style = [System.Windows.Forms.ProgressBarStyle]::Marquee
$progress.MarqueeAnimationSpeed = 30
$progress.Visible = $false
$form.Controls.Add($progress)

$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = [System.Windows.Forms.ScrollBars]::Vertical
$log.WordWrap = $false
$log.Font = New-Object System.Drawing.Font("Consolas", 8.5)
$log.Location = New-Object System.Drawing.Point(16, 462)
$log.Size = New-Object System.Drawing.Size(608, 166)
$form.Controls.Add($log)

function Write-Log($text) { $log.AppendText($text + "`r`n") }

# Start-Process joins its arguments with spaces, unquoted: quote each one (paths can have spaces).
function Join-Args($list) {
    ($list | ForEach-Object { if ($_ -eq "" -or $_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join " "
}

# State ---------------------------------------------------------------------------
$script:job = $null                 # the running setup / build-tools install
$script:outPath = $null; $script:errPath = $null
$script:outPos = 0; $script:errPos = 0
$script:jobKind = ""

function Update-Status {
    $busy = $null -ne $script:job
    $tools = Test-BuildTools
    $toolsStatus.Text = if ($tools) { "$Ok  Visual Studio C++ build tools and Clang found." } else { "$No  Not found: Install Build Tools (free, about 6 GB, takes a while)." }
    $toolsStatus.ForeColor = if ($tools) { [System.Drawing.Color]::DarkGreen } else { [System.Drawing.Color]::Firebrick }
    $toolsButton.Enabled = -not $busy -and -not $tools
    $disc = Test-Disc
    $built = Test-Built
    $discStatus.Text = if ((Test-Path (Join-Path (Get-DiscFolder) "sys\main.dol"))) { "$Ok  Unpacked in $(Get-DiscFolder)." } elseif ($disc) { "$Ok  Set Up will unpack it." } else { "$No  Pick your disc image." }
    $discStatus.ForeColor = if ($disc) { [System.Drawing.Color]::DarkGreen } else { [System.Drawing.Color]::Firebrick }
    $ps2 = $ps2Box.Text.Trim().Trim('"')
    if (Test-PS2Extras) { $ps2Status.Text = "$Ok  Installed: Previews and the PS2 intro movies."; $ps2Status.ForeColor = [System.Drawing.Color]::DarkGreen }
    elseif ($ps2 -ne "" -and (Test-Path $ps2 -PathType Leaf)) { $ps2Status.Text = "Set Up will add them (downloads ffmpeg, about 200 MB, to convert the movies)."; $ps2Status.ForeColor = [System.Drawing.Color]::DimGray }
    elseif ($ps2 -ne "") { $ps2Status.Text = "$No  That file doesn't exist."; $ps2Status.ForeColor = [System.Drawing.Color]::Firebrick }
    else { $ps2Status.Text = "Not added: without it, Previews is hidden and the intros are the GameCube's."; $ps2Status.ForeColor = [System.Drawing.Color]::DimGray }
    $ps2Box.Enabled = -not $busy; $ps2Button.Enabled = -not $busy
    $setupButton.Enabled = -not $busy -and $tools -and $disc
    $ps2Pending = $ps2 -ne "" -and (Test-Path $ps2 -PathType Leaf) -and -not (Test-PS2Extras)
    $setupButton.Text = if ($built -and $ps2Pending) { "Add PS2 Extras" } elseif ($built) { "Set Up Again" } else { "Set Up" }
    $playButton.Enabled = -not $busy -and $built
    $discBox.Enabled = -not $busy; $discButton.Enabled = -not $busy
    $rootBox.Enabled = -not $busy; $rootButton.Enabled = -not $busy; $shortcutBox.Enabled = -not $busy
    if (-not $busy) {
        $step.Text = if ($built -and $ps2Pending) { "Ready to play, or add the PS2 extras." } elseif ($built) { "Ready to play." } elseif ($tools -and $disc) { "Ready to set up (several minutes)." } else { "" }
    }
}

# Reads what the running job has written since last time (both of its outputs).
function Read-New($path, [ref]$pos) {
    if (-not $path -or -not (Test-Path $path)) { return }
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            if ($fs.Length -le $pos.Value) { return }
            $fs.Position = $pos.Value
            $buf = New-Object byte[] ($fs.Length - $pos.Value)
            $n = $fs.Read($buf, 0, $buf.Length)
            $text = [System.Text.Encoding]::Default.GetString($buf, 0, $n)
            $cut = $text.LastIndexOf("`n")
            if ($cut -lt 0) { return }               # (wait for the whole line)
            $pos.Value += [System.Text.Encoding]::Default.GetByteCount($text.Substring(0, $cut + 1))
            foreach ($line in $text.Substring(0, $cut).Split("`n")) {
                $line = $line.TrimEnd("`r")
                if ($line -match '^== (.*)$') { $step.Text = $Matches[1] + "..." }
                elseif ($line -match '^Converting movie (\d+) of (\d+)') { $step.Text = "Converting movie $($Matches[1]) of $($Matches[2])..." }
                elseif ($line -match '^\[(\d+)/(\d+)\]') { $step.Text = "Building ($($Matches[1]) of $($Matches[2]))..." }
                Write-Log $line
            }
        } finally { $fs.Close() }
    } catch { }
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 250
$timer.Add_Tick({
    if (-not $script:job) { return }
    Read-New $script:outPath ([ref]$script:outPos)
    Read-New $script:errPath ([ref]$script:errPos)
    if ($script:job.HasExited) {
        Read-New $script:outPath ([ref]$script:outPos)
        Read-New $script:errPath ([ref]$script:errPos)
        $code = $script:job.ExitCode
        $kind = $script:jobKind
        $script:job = $null
        $progress.Visible = $false
        Update-Status
        if ($kind -eq "setup") {
            if ($code -eq 0 -and (Test-Built)) {
                $step.Text = "Done: ready to play."
                Write-Log ""
                Write-Log "Set up. Press Play."
                if (Test-Path $icon) { try { $form.Icon = New-Object System.Drawing.Icon($icon) } catch { } }
            } else {
                $step.Text = "Set up failed (see below)."
                [System.Windows.Forms.MessageBox]::Show($form, "Setting up didn't finish. The reason is at the end of the log.", "NFL Street 2 Recomp", "OK", "Warning") | Out-Null
            }
        } elseif ($kind -eq "tools") {
            if (Test-BuildTools) { $step.Text = "Build tools installed." } else { $step.Text = "The build tools aren't there yet." }
        }
    }
})
$timer.Start()

function Start-Job2($kind, $arguments) {
    $tmp = Join-Path $env:TEMP "nfls2-launcher"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $script:outPath = Join-Path $tmp "$kind-out.txt"
    $script:errPath = Join-Path $tmp "$kind-err.txt"
    Remove-Item $script:outPath, $script:errPath -ErrorAction SilentlyContinue
    $script:outPos = 0; $script:errPos = 0
    $script:jobKind = $kind
    $log.Clear()
    $progress.Visible = $true
    $script:job = Start-Process powershell -ArgumentList (Join-Args $arguments) -WorkingDirectory $PSScriptRoot -WindowStyle Hidden `
        -RedirectStandardOutput $script:outPath -RedirectStandardError $script:errPath -PassThru
    if ($script:job) { $null = $script:job.Handle }   # (keeps the exit code readable once it ends)
    Update-Status
}

# Buttons ---------------------------------------------------------------------------
$toolsButton.Add_Click({
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        [System.Windows.Forms.MessageBox]::Show($form, "Install 'Build Tools for Visual Studio 2022' from the page that opens, with 'Desktop development with C++' and its 'C++ Clang tools for Windows' option. Then come back here.", "NFL Street 2 Recomp", "OK", "Information") | Out-Null
        Start-Process "https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022"
        return
    }
    $r = [System.Windows.Forms.MessageBox]::Show($form, "This installs Microsoft's free 'Build Tools for Visual Studio 2022' (C++ and Clang, about 6 GB). Windows asks for permission, and it can take 10 to 30 minutes. Go ahead?", "NFL Street 2 Recomp", "OKCancel", "Question")
    if ($r -ne [System.Windows.Forms.DialogResult]::OK) { return }
    $override = "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset"
    $cmd = "winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override '$override'; exit `$LASTEXITCODE"
    Write-Log "Installing the build tools (the Visual Studio Installer window shows its progress)..."
    Start-Job2 "tools" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $cmd)
    $step.Text = "Installing the build tools..."
})

$discButton.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Pick your NFL Street 2 (GameCube) disc image"
    $dlg.Filter = "GameCube disc images (*.iso;*.gcm)|*.iso;*.gcm|All files (*.*)|*.*"
    $cur = $discBox.Text.Trim().Trim('"')
    if ($cur -ne "" -and (Test-Path (Split-Path $cur -Parent) -ErrorAction SilentlyContinue)) { $dlg.InitialDirectory = Split-Path $cur -Parent }
    if ($dlg.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
        $discBox.Text = $dlg.FileName
        Save-Config
        Update-Status
    }
})
$discBox.Add_Leave({ Save-Config; Update-Status })

$ps2Button.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Pick your NFL Street 2 (PS2) disc image"
    $dlg.Filter = "PS2 disc images (*.iso)|*.iso|All files (*.*)|*.*"
    if ($dlg.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
        $ps2Box.Text = $dlg.FileName
        Save-Config
        Update-Status
    }
})
$ps2Box.Add_Leave({ Save-Config; Update-Status })

$rootButton.Add_Click({
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Where to put the unpacked disc, the recompiled code, saves and settings"
    $dlg.SelectedPath = Get-Root
    if ($dlg.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
        $rootBox.Text = $dlg.SelectedPath
        Save-Config
        Update-Status
    }
})
$rootBox.Add_Leave({ Save-Config; Update-Status })
$shortcutBox.Add_CheckedChanged({ Save-Config })
$texButton.Add_Click({
    # (made if it isn't there yet; custom texture packs go in it)
    $t = Join-Path (Get-Root) "textures"
    New-Item -ItemType Directory -Force -Path $t | Out-Null
    Start-Process explorer.exe -ArgumentList ('"' + $t + '"')
})

$setupButton.Add_Click({
    Save-Config
    $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "setup.ps1"), "-Root", (Get-Root))
    $d = $discBox.Text.Trim().Trim('"')
    if ($d -ne "") { $a += @("-Disc", $d) }
    elseif (Test-Path (Join-Path (Get-DiscFolder) "sys\main.dol")) { $a += @("-Disc", (Get-DiscFolder)) }
    $p2 = $ps2Box.Text.Trim().Trim('"')
    if ($p2 -ne "" -and (Test-Path $p2 -PathType Leaf)) { $a += @("-PS2Disc", $p2) }
    if (-not $shortcutBox.Checked) { $a += "-NoShortcut" }
    Start-Job2 "setup" $a
    $step.Text = "Starting..."
})

$playButton.Add_Click({
    Save-Config
    $a = @()
    if ((Get-DiscFolder) -ne "C:\nfls2\disc") { $a += @("-Disc", (Get-DiscFolder)) }
    $r = Get-Root
    if ($r -ne "C:\nfls2") {   # (settings, saves and textures in the install folder too)
        $a += @("-Settings", (Join-Path $r "settings.ini"), "-MemCard", (Join-Path $r "saves\MemoryCardA.raw"),
                "-Textures", (Join-Path $r "textures"), "-Replace", (Join-Path $r "replace"))
    }
    $bat = Join-Path $PSScriptRoot "play.bat"   # (it stays open if something goes wrong)
    if ($a.Count) { Start-Process $bat -ArgumentList (Join-Args $a) -WorkingDirectory $PSScriptRoot -WindowStyle Minimized | Out-Null }
    else { Start-Process $bat -WorkingDirectory $PSScriptRoot -WindowStyle Minimized | Out-Null }
    $step.Text = "Starting the game..."
})

$form.Add_FormClosing({
    param($s, $e)
    if ($script:job -and $script:jobKind -eq "setup" -and -not $script:job.HasExited) {
        $r = [System.Windows.Forms.MessageBox]::Show($form, "Setting up is still running. Stop it and close?", "NFL Street 2 Recomp", "YesNo", "Warning")
        if ($r -ne [System.Windows.Forms.DialogResult]::Yes) { $e.Cancel = $true; return }
        try { & taskkill /T /F /PID $script:job.Id | Out-Null } catch { }
    }
    Save-Config
})

Update-Status
if (-not (Test-BuildTools)) { Write-Log "Start with 1: the build tools. They're Microsoft's free C++ compiler, which builds the port on your PC." }
elseif (-not (Test-Built)) { Write-Log "Pick your disc, then press Set Up. The first set up takes several minutes." }
[void]$form.ShowDialog()
