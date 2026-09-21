param([string]$ManagerRoot, [switch]$LibraryOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Utf8 = New-Object Text.UTF8Encoding($false)
function Write-Atomic([string]$Path, [string]$Text) {
    $temp = $Path + '.tmp'
    $bytes = $script:Utf8.GetBytes($Text)
    $f = [IO.File]::Open($temp, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $f.Write($bytes, 0, $bytes.Length); $f.Flush($true) } finally { $f.Dispose() }
    for($attempt=0; ; $attempt++) {
        try {
            if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temp, $Path, [NullString]::Value) }
            else { [IO.File]::Move($temp, $Path) }
            break
        } catch { if($attempt -ge 20) { throw }; Start-Sleep -Milliseconds 25 }
    }
}
function Assert-Child([string]$Root, [string]$Path) {
    $r = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $p = [IO.Path]::GetFullPath($Path)
    if (!$p.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)) { throw "Path outside expected root: $p" }
    return $p
}
function Assert-PlainAncestors([string]$Root, [string]$Path) {
    $p = Assert-Child $Root $Path
    while ($p.Length -gt $Root.TrimEnd('\').Length) {
        if (Test-Path -LiteralPath $p) {
            if ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Unexpected reparse path: $p" }
        }
        $p = [IO.Path]::GetDirectoryName($p)
    }
}
function Validate-Item($Item, [string]$Root) {
    if ($Item.name -notmatch '^[^\\/:*?"<>|\t\r\n]+$' -or $Item.name -in @('.', '..')) { throw 'Invalid mod name' }
    if ($Item.rel -match '(^|[\\/])\.\.?([\\/]|$)|[:\t\r\n]' -or [IO.Path]::IsPathRooted($Item.rel)) { throw 'Invalid relative path' }
    $src = Assert-Child (Join-Path $Root 'plugins') (Join-Path (Join-Path $Root 'plugins') $Item.rel)
    Assert-PlainAncestors (Join-Path $Root 'plugins') $src
    if ($Item.kind -notin @('script','pak') -or $Item.target -notin @('~mods','LogicMods')) { throw 'Invalid mod kind/target' }
    if ($Item.id -cne ($Item.kind + '|' + $Item.rel)) { throw 'Invalid mod identity' }
    if (!(Test-Path -LiteralPath $src -PathType Container)) { return $false }
    if ($Item.kind -eq 'script') {
        return ((Test-Path -LiteralPath "$src\Scripts\main.lua") -or (Test-Path -LiteralPath "$src\dlls\main.dll"))
    }
    return (@(Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -in @('.pak','.utoc','.ucas') }).Count -gt 0)
}
function Get-Fingerprint($Item, [string]$Root) {
    $src = Join-Path (Join-Path $Root 'plugins') $Item.rel
    # Include code/content, exclude all settings: user changes must never be reset.
    $files = if ($Item.kind -eq 'pak') { @(Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -in @('.pak','.utoc','.ucas') }) }
             else { @(Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $_.Extension -in @('.dll','.lua') }) }
    return (($files | Sort-Object FullName | ForEach-Object { $_.FullName + ':' + $_.Length + ':' + $_.LastWriteTimeUtc.Ticks }) -join '|')
}
function Disable-ModsTxt([string]$Path, [string]$Name) {
    if (!(Test-Path -LiteralPath $Path)) { return }
    $text = [IO.File]::ReadAllText($Path)
    $pattern = '(?m)^(\s*' + [regex]::Escape($Name) + '\s*:\s*)1(?=\s*(?:;[^\r\n]*)?$)'
    $changed = [regex]::Replace($text, $pattern, '${1}0')
    if ($changed -cne $text) { Write-Atomic $Path $changed }
}
function Set-ModState($Item, [bool]$On, [string]$Root) {
    $src = Assert-Child "$Root\plugins" (Join-Path "$Root\plugins" $Item.rel)
    Assert-PlainAncestors "$Root\plugins" $src
    $mods = [IO.Path]::GetDirectoryName($Root)
    $marker = Join-Path $src 'enabled.txt'
    if ($Item.kind -eq 'script') {
        $entry = Assert-Child $mods (Join-Path $mods $Item.name)
        if (Test-Path -LiteralPath $entry) {
            $e = Get-Item -LiteralPath $entry -Force
            if (!($e.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Refuse legacy copy: $entry" }
            $target = [IO.Path]::GetFullPath([string]@($e.Target)[0]).TrimEnd('\')
            if (!$target.Equals($src.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { throw "Foreign mod entry: $entry" }
        }
        if ($On) {
            if (!(Test-Path -LiteralPath $entry)) { New-Item -ItemType Junction -Path $entry -Target $src | Out-Null }
            Write-Atomic $marker ''
        } else {
            if ([IO.File]::Exists($marker)) { [IO.File]::Delete($marker) }
            Disable-ModsTxt (Join-Path $mods 'mods.txt') $Item.name
            if (Test-Path -LiteralPath $entry) { [IO.Directory]::Delete($entry, $false) } # junction only, NEVER recursive
        }
    } else {
        $ds = [IO.Path]::GetFullPath((Join-Path $Root '..\..\..\..\..'))
        $paks = Join-Path $ds 'Content\Paks'
        foreach ($sub in @('~mods','LogicMods')) {
            $dst = Assert-Child $paks (Join-Path $paks $sub)
            Assert-PlainAncestors $paks $dst
            if (!(Test-Path -LiteralPath $dst)) { New-Item -ItemType Directory -Path $dst | Out-Null }
            foreach ($file in @(Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -in @('.pak','.utoc','.ucas') })) {
                if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unexpected pak reparse file' }
                $link = Assert-Child $dst (Join-Path $dst $file.Name)
                $exists = [IO.File]::Exists($link)
                if ($exists -and !(Test-SameFile $file.FullName $link)) { throw "Foreign pak file: $link" }
                if ($On -and $sub -eq $Item.target) {
                    if (!$exists) { New-Item -ItemType HardLink -Path $link -Target $file.FullName | Out-Null }
                } elseif ($exists) { [IO.File]::Delete($link) }
            }
        }
        if ($On) { Write-Atomic $marker '' } elseif ([IO.File]::Exists($marker)) { [IO.File]::Delete($marker) }
    }
    Write-Atomic (Join-Path $src 'dsruntime.txt') $(if ($On) { '1' } else { '0' })
}
# File identity, not equal contents: never unlink a user's unrelated pak file.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class DsRecoveryFiles {
 [StructLayout(LayoutKind.Sequential)] public struct Info { public uint attr; public System.Runtime.InteropServices.ComTypes.FILETIME c,a,w; public uint volume,hi,lo,links,indexHi,indexLo; }
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetFileInformationByHandle(SafeFileHandle h,out Info i);
}
"@
function Test-SameFile([string]$A, [string]$B) {
    $fa=$null; $fb=$null
    try {
        $fa=[IO.File]::Open($A,'Open','Read','ReadWrite,Delete'); $fb=[IO.File]::Open($B,'Open','Read','ReadWrite,Delete')
        $ia=New-Object DsRecoveryFiles+Info; $ib=New-Object DsRecoveryFiles+Info
        if (![DsRecoveryFiles]::GetFileInformationByHandle($fa.SafeFileHandle,[ref]$ia) -or ![DsRecoveryFiles]::GetFileInformationByHandle($fb.SafeFileHandle,[ref]$ib)) { return $false }
        return ($ia.volume -eq $ib.volume -and $ia.indexHi -eq $ib.indexHi -and $ia.indexLo -eq $ib.indexLo)
    } finally { if($fa){$fa.Dispose()}; if($fb){$fb.Dispose()} }
}
function Read-Kv([string]$Path) {
    $out=@{}
    if (Test-Path -LiteralPath $Path) {
        foreach($line in [IO.File]::ReadAllLines($Path)) { $i=$line.IndexOf('='); if($i -gt 0) { $out[$line.Substring(0,$i)]=$line.Substring($i+1) } }
    }
    return $out
}
function Has-Crash([string]$Root, [datetime]$Since) {
    $ue=[IO.Path]::GetDirectoryName([IO.Path]::GetDirectoryName($Root))
    $ds=[IO.Path]::GetFullPath((Join-Path $Root '..\..\..\..\..'))
    foreach($p in @($ue, (Join-Path $ds 'Saved\Crashes'))) {
        if (!(Test-Path -LiteralPath $p)) { continue }
        $found=@(Get-ChildItem -LiteralPath $p -Force | Where-Object { $_.LastWriteTimeUtc -gt $Since -and ($p -ne $ue -or $_.Name -like 'crash_*.dmp') })
        if($found.Count) { return $true }
    }
    return $false
}
function Get-ExitVerdict($Process, [string]$Root, [datetime]$Since) {
    $artifact=Has-Crash $Root $Since
    $code=$Process.ExitCode
    if($artifact) { return 'crash' }
    if($null -eq $code) { return 'unknown' }
    $bits=[BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$code),0)
    if($bits -in @(0xC0000005L,0xC000001DL,0xC0000094L,0xC00000FDL,0xC0000409L,0xC0000374L,0x40000015L,0xE06D7363L,0x80000003L)) { return 'crash' }
    if($code -eq 0) { return 'normal' }
    return 'unknown'
}
if ($LibraryOnly) { return }
$ManagerRoot=[IO.Path]::GetFullPath($ManagerRoot).TrimEnd('\')
$script:Base=Join-Path $ManagerRoot 'autorecovery'
$plan=Get-Content -LiteralPath "$script:Base\plan.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$session=[string]$plan.session
if($session -notmatch '^[0-9a-f-]{36}$') { throw 'Invalid session' }
$script:Rows=@($plan.items)
$script:Index=-1
$script:GamePid=0
$script:Phase='starting'
$script:Note=''
$script:Started=[datetime]::UtcNow
$script:PriorPending=@()
$journal=Join-Path $ManagerRoot 'safemode_restore.txt'
if(Test-Path -LiteralPath $journal) { $script:PriorPending=@([IO.File]::ReadAllLines($journal) | Where-Object { $_ -and !($_.StartsWith('#')) -and $_ -notin $script:Rows.id }) }
$script:Rows | ForEach-Object { $_ | Add-Member -NotePropertyName verdict -NotePropertyValue 'queued'; $_ | Add-Member -NotePropertyName fingerprint -NotePropertyValue '' }
function Save-State([string]$Phase,[string]$Note='') {
    $script:Phase=$Phase; $script:Note=$Note
    $current=if($script:Index -ge 0 -and $script:Index -lt $script:Rows.Count){$script:Rows[$script:Index].name}else{''}
    $data=@{session=$session;phase=$Phase;pid=$script:GamePid;helper_pid=$PID;index=$script:Index;items=$script:Rows;note=$Note;utc=[datetime]::UtcNow.ToString('o')}
    Write-Atomic "$script:Base\state.json" ($data | ConvertTo-Json -Depth 6)
    Write-Atomic "$script:Base\status.txt" "session=$session`nphase=$Phase`npid=$script:GamePid`nhelper_pid=$PID`ncurrent=$current`nindex=$([Math]::Min($script:Rows.Count,$script:Index+1))`ntotal=$($script:Rows.Count)`npassed=$(@($script:Rows|Where-Object verdict -eq 'passed').Count)`nsuspect=$(@($script:Rows|Where-Object verdict -eq 'suspect').Count)`nsuspect_names=$((@($script:Rows|Where-Object verdict -eq 'suspect'|ForEach-Object name)) -join ", ")`nnote=$Note`n"
}
function Assert-GameStopped([string]$Exe) {
    foreach($other in @(Get-Process -Name '*-Win64-Shipping' -ErrorAction SilentlyContinue)) {
        if($other.Path -eq $Exe -and !$other.HasExited) { throw 'another-game-process-is-running' }
    }
}
function Check-Stop {
    $settings=Read-Kv "$script:Base\settings.txt"
    if($settings['enabled'] -eq '0') { throw 'user-stop' }
    if(Test-Path -LiteralPath "$script:Base\stop.txt") { throw 'user-stop' }
}
function Request-Exit($Process,[string]$Why) {
    Write-Atomic "$script:Base\command.txt" "session=$session`npid=$($Process.Id)`naction=quit`n"
    $until=[datetime]::UtcNow.AddSeconds(60)
    while(!$Process.HasExited -and [datetime]::UtcNow -lt $until) { Check-Stop; Start-Sleep -Milliseconds 250; $Process.Refresh() }
    if(!$Process.HasExited) { throw "normal-exit-timeout:$Why" }
    [IO.File]::Delete("$script:Base\command.txt")
}
function Save-Pending {
    $pending=@($script:PriorPending) + @($script:Rows | Where-Object { $_.verdict -in @('queued','unavailable') } | ForEach-Object id)
    Write-Atomic (Join-Path $ManagerRoot 'safemode_restore.txt') ("# DsCppModManager recovery v1`n" + ($pending -join "`n") + "`n")
    $suspects=@($script:Rows | Where-Object verdict -eq 'suspect' | ForEach-Object id)
    Write-Atomic "$script:Base\suspects.txt" (($suspects -join "`n") + "`n")
}
$mutex=$null
$owned=$false
try {
    $sha=[Security.Cryptography.SHA256]::Create()
    $key=[BitConverter]::ToString($sha.ComputeHash($script:Utf8.GetBytes($ManagerRoot.ToLowerInvariant()))).Replace('-',''); $sha.Dispose()
    $mutex=New-Object Threading.Mutex($false, ('Local\DsMmRecovery-'+$key))
    $owned=$mutex.WaitOne(0)
    if(!$owned) { throw 'already-running' }
    $exe=[IO.Path]::GetFullPath([string]$plan.exe)
    $win64=[IO.Path]::GetFullPath((Join-Path $ManagerRoot '..\..\..'))
    if([IO.Path]::GetDirectoryName($exe) -ne $win64 -or [IO.Path]::GetFileName($exe) -notlike '*-Win64-Shipping.exe') { throw 'Unexpected game executable' }
    $exeStamp=(Get-Item -LiteralPath $exe).LastWriteTimeUtc.Ticks.ToString()+':'+(Get-Item -LiteralPath $exe).Length
    $game=Get-Process -Id ([int]$plan.initial_pid) -ErrorAction Stop
    if($game.Path -ne $exe) { throw 'Initial process identity mismatch' }
    if($script:Rows.Count -gt 1000) { throw 'Invalid candidate count' }
    $unique=@{}
    foreach($item in $script:Rows) {
        if($unique.ContainsKey($item.id)) { throw 'Duplicate candidate' }; $unique[$item.id]=$true
        if(!(Validate-Item $item $ManagerRoot)) { $item.verdict='unavailable' }
        else { $item.fingerprint=Get-Fingerprint $item $ManagerRoot }
    }
    $script:GamePid=$game.Id
    $null=$game.Handle # keep an exit-code-capable handle before termination
    $watchOnly=($plan.PSObject.Properties.Name -contains 'mode' -and $plan.mode -eq 'watch')
    if($watchOnly) {
        $since=$game.StartTime.ToUniversalTime()
        Save-State 'watching' 'normal-exit-does-not-change-mods'
        while(!$game.HasExited) { Check-Stop; Start-Sleep -Milliseconds 500; $game.Refresh() }
        $game.WaitForExit(); Start-Sleep -Seconds 2
        $verdict=Get-ExitVerdict $game $ManagerRoot $since
        if($verdict -eq 'normal') { Save-State 'armed' 'normal-exit-no-mod-changes'; return }
        if($verdict -ne 'crash') { throw 'unknown-exit-no-quarantine' }
        if($script:Rows.Count -eq 0) { throw 'crash-with-no-enabled-managed-mods' }
        Save-State 'preparing' 'confirmed-crash-starting-sequential-checks'
        Save-Pending
    } else {
        Save-State 'preparing' 'normal-restart-required'
        Request-Exit $game 'initial'
    }
    Check-Stop
    for($script:Index=0; $script:Index -lt $script:Rows.Count; $script:Index++) {
        Check-Stop
        $item=$script:Rows[$script:Index]
        if($item.verdict -ne 'queued') { continue }
        if(((Get-Item -LiteralPath $exe).LastWriteTimeUtc.Ticks.ToString()+':'+(Get-Item -LiteralPath $exe).Length) -ne $exeStamp) { throw 'game-updated-during-test' }
        foreach($candidate in $script:Rows) {
            if($candidate.verdict -eq 'unavailable') { continue }
            if(!(Validate-Item $candidate $ManagerRoot) -or (Get-Fingerprint $candidate $ManagerRoot) -cne $candidate.fingerprint) { throw 'mod-files-changed-during-test' }
        }
        Assert-GameStopped $exe
        # Commit current candidate before any load flag change. Only modify stopped processes.
        $script:GamePid=0
        Save-State 'preparing' 'applying-one-candidate'
        foreach($candidate in $script:Rows) {
            Check-Stop
            if($candidate.verdict -eq 'unavailable') { continue }
            Set-ModState $candidate ($candidate.verdict -eq 'passed' -or $candidate.id -eq $item.id) $ManagerRoot
        }
        Save-Pending
        foreach($file in @('heartbeat.txt','command.txt')) { if(Test-Path -LiteralPath "$script:Base\$file") { [IO.File]::Delete("$script:Base\$file") } }
        Check-Stop
        $launch=[datetime]::UtcNow
        Save-State 'launching' 'enter-game-to-start-5-minute-check'
        $game=Start-Process -FilePath $exe -WorkingDirectory $win64 -WindowStyle Normal -PassThru
        $null=$game.Handle
        $script:GamePid=$game.Id
        Save-State 'testing' 'waiting-for-gameplay'
        $playSeconds=0.0
        $lastBeat=''
        $lastTick=[datetime]::UtcNow
        $wasPlaying=$false
        $passed=$false
        while(!$game.HasExited) {
            Check-Stop
            Start-Sleep -Milliseconds 500
            $game.Refresh()
            $now=[datetime]::UtcNow
            $beat=Read-Kv "$script:Base\heartbeat.txt"
            if($beat['session'] -eq $session -and $beat['pid'] -eq [string]$game.Id -and $beat['playing'] -eq '1' -and $beat['tick'] -ne $lastBeat) {
                # At most one second per fresh game-thread heartbeat. Loading, pause and hangs do not count.
                if($wasPlaying) { $playSeconds += [Math]::Min(1.5, ($now-$lastTick).TotalSeconds) }
                $lastTick=$now
                $wasPlaying=$true
                $lastBeat=$beat['tick']
            }
            if($beat['playing'] -ne '1') { $wasPlaying=$false; $lastTick=$now }
            if($playSeconds -ge 300) { $passed=$true; Save-State 'restarting' '5-minute-check-finished'; Request-Exit $game 'next-candidate'; break }
            if(($now-$launch).TotalMinutes -gt 30) { throw 'no-test-completion-within-30-minutes' }
        }
        $game.WaitForExit()
        Start-Sleep -Seconds 2 # allow crash reporter metadata to settle
        $verdict=Get-ExitVerdict $game $ManagerRoot $launch
        if($verdict -eq 'unknown') { throw 'unknown-exit-no-quarantine' }
        if($verdict -eq 'crash') { $item.verdict='suspect'; Save-State 'quarantining' 'crash-while-testing-not-proof'; Set-ModState $item $false $ManagerRoot }
        elseif($passed) { $item.verdict='passed'; Save-State 'passed' 'five-minute-observation-only' }
        else { throw 'normal-exit-before-test-completion' }
        Save-Pending
    }
    $script:GamePid=0
    Save-State 'complete' 'passed-mods-enabled-suspects-off'
} catch {
    if($owned) { try { Save-State 'paused' $_.Exception.Message } catch {} }
} finally {
    if($owned) { $mutex.ReleaseMutex() }
    if($mutex) { $mutex.Dispose() }
}
