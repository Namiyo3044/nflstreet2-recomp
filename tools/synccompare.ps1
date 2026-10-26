# Compares two sync logs (written with --sync-log) and says where the runs parted:
# the first second of game time where game memory differs, and which 64 KB blocks.
#   .\tools\synccompare.ps1 C:\nfls2\netsync\sync-record.txt C:\nfls2\netsync\sync-replay.txt
param([string]$A, [string]$B)

function Read-Log([string]$path) {
    $rows = @{}
    foreach ($line in Get-Content $path) {
        if ($line -notmatch '^field (\d+) answers (\d+) all ([0-9a-f]+) chunks (.*)$') { continue }
        $rows[[long]$Matches[1]] = [pscustomobject]@{
            Answers = [long]$Matches[2]; All = $Matches[3]; Chunks = $Matches[4].Split(' ')
        }
    }
    return $rows
}

$ra = Read-Log $A
$rb = Read-Log $B
$fields = @($ra.Keys | Where-Object { $rb.ContainsKey($_) } | Sort-Object)
if ($fields.Count -eq 0) { Write-Host "sync: no seconds in common between the two logs"; exit 1 }

$first = $null
$differ = 0
foreach ($f in $fields) {
    if ($ra[$f].All -ne $rb[$f].All) {
        $differ++
        if ($null -eq $first) { $first = $f }
    }
}
$secs = { param($f) "{0:N1} s" -f ($f * 1001.0 / 60000.0) }
Write-Host ("sync: compared {0} seconds of game time (up to {1})" -f $fields.Count, (& $secs $fields[-1]))
if ($null -eq $first) {
    Write-Host "sync: identical all the way: the run repeated exactly"
    exit 0
}
Write-Host ("sync: game memory first differs at {0} (video field {1}); {2} of {3} seconds differ" -f (& $secs $first), $first, $differ, $fields.Count)
$prev = ($fields | Where-Object { $_ -lt $first } | Select-Object -Last 1)
if ($prev) { Write-Host ("sync: the last second that matched: {0}" -f (& $secs $prev)) }
if ($ra[$first].Answers -ne $rb[$first].Answers) {
    Write-Host ("sync: controller answers by then: {0} vs {1} (the game asked differently)" -f $ra[$first].Answers, $rb[$first].Answers)
}
$ca = $ra[$first].Chunks; $cb = $rb[$first].Chunks
$blocks = @()
for ($i = 0; $i -lt [Math]::Min($ca.Count, $cb.Count); $i++) {
    if ($ca[$i] -ne $cb[$i]) { $blocks += ("0x{0:X8}" -f (0x80000000L + $i * 0x10000)) }
}
Write-Host ("sync: {0} blocks of 64 KB differ there: {1}" -f $blocks.Count, (($blocks | Select-Object -First 40) -join ' '))
# blocks that differ in every later second (lasting differences, not one-off)
$always = @()
for ($i = 0; $i -lt $ca.Count; $i++) {
    $all = $true
    foreach ($f in ($fields | Where-Object { $_ -ge $first })) {
        if ($ra[$f].Chunks[$i] -eq $rb[$f].Chunks[$i]) { $all = $false; break }
    }
    if ($all) { $always += ("0x{0:X8}" -f (0x80000000L + $i * 0x10000)) }
}
Write-Host ("sync: blocks that differ from then on in every second: {0}" -f (($always | Select-Object -First 40) -join ' '))
