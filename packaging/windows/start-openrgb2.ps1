<#
.SYNOPSIS
    Managed startup for an installed OpenRGB2 release (Task 6.2).

.DESCRIPTION
    Starts the managed stack in dependency order:

      1. ownership scan -- the SAME classification rules as the engine's
         openrgb2::ClassifyOwners (src/engine/OwnershipScan.cpp):
         competing RGB owners (other OpenRGB instances, L-Connect,
         GCC/RGB Fusion, Synapse, G Hub, iCUE, SignalRGB) are REPORTED;
         FanControl is an expected notice (it owns cooling, not RGB).
      2. takeover gate -- when conflicts exist, startup REFUSES unless the
         operator passes -TakeOver. That is the explicit, user-visible
         takeover step required by plan section 4: takeover is never
         silent and this script NEVER kills a competing process (there
         is deliberately no Stop-Process call in this file).
      3. backend -- launches the pinned vendored backend:

           backend\OpenRGB.exe --server --server-host 127.0.0.1
             --server-port 6744 --noautoconnect --localconfig

         with the private config dir state\backend as its working
         directory (the engine attaches over the loopback SDK; it
         never spawns the backend -- this launcher owns that job).
         When port 6744 is already held by OUR backend the running
         instance is reused; when a FOREIGN process holds it that is a
         takeover-relevant conflict (with -TakeOver the engine attaches
         to whatever listens and reports it honestly; our backend is
         then not spawned -- the port cannot be shared).
         The pinned binary carries its OWN single-instance guard
         (vendored startup\main_Windows.cpp): launched while a foreign
         OpenRGB.exe lives it shows a Yes/No prompt whose Yes
         hard-kills that instance via TerminateProcess (and its
         server probe checks the legacy default port 6742, not our
         managed 6744). The vendored cli has NO flag to skip that
         guard, so this launcher refuses the backend spawn while a
         foreign OpenRGB.exe is running -- exit 3, close it yourself;
         the kill-capable prompt is never triggered from here. An
         OpenRGB.exe at OUR pinned path is a still-starting managed
         backend: waited on, never double-spawned.
      4. engine -- openrgb2-engine.exe --sdk with the private config dir
         passed as --backend-config so startup runs the managed-config
         sole-writer check (legacy Studio/Effects/HardwareSync
         producers reported on both plugin-load paths).
      5. UI -- openrgb2-ui.exe (skip with -NoUI). The engine keeps
         running when the UI closes (UI independence, plan section 6).

    First-run behavior: profiles\local\ does not exist yet -- the
    engine's ProfileStore treats a missing profile as a fresh start
    (no error), writes state\ and profiles\local\ on first save, and
    migrates older schema versions forward. With no --scene the engine
    restores profiles\local\last-scene.json -- the most recent
    NON-EMPTY applied scene, snapshotted after every successful
    apply (an empty scene never overwrites it). Only a first-ever
    run or -NoSceneRestore leaves the engine idling at the 60 Hz
    scene clock until the UI applies a scene.

    Run state is recorded in state\openrgb2.run.json (PIDs + image
    paths) so stop-openrgb2.ps1 can order the shutdown and verify it
    only ever closes OUR processes.

    Unverifiable processes: from a NON-elevated shell the image path
    of an elevated process cannot be read, so an elevated managed
    stack is indistinguishable from a foreign one. A same-named
    process (OpenRGB.exe / openrgb2-engine.exe, or the :6744 listener)
    whose image path is unreadable is therefore never classified as
    foreign and never ignored: startup REFUSES with exit 4 (even with
    -TakeOver) and asks for an elevated re-run -- the same rule as
    stop-openrgb2.ps1. Without that rule our own backend became a
    "foreign listener" and a second engine was spawned.

    Logs: state\startup.log and state\shutdown.log (written by the
    desktop UI) are renamed to <name>.1 once they exceed 1 MB -- only
    while no process holds them open (else skipped until a later
    start); never truncated in place.

    Output: every finding, CONFLICT / UNVERIFIABLE / refusal line goes
    to the host (Write-Host), which is this process's STDOUT when run
    with -File -- the desktop UI parses it from there. Callers must
    capture stdout; the script does not write any log file itself.

    Exit codes: 0 started/attached, 1 fatal error (missing files),
    3 refused (conflicts without -TakeOver, or a foreign OpenRGB.exe
    blocking the safe backend spawn), 4 refused (same-named
    processes whose image path is unreadable from this shell --
    re-run elevated).

    This file is dot-source safe: sourcing it defines the functions
    without running the startup (the offline packaging suite uses
    that seam -- the same role as EngineHost's injected options).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\start-openrgb2.ps1

.EXAMPLE
    # explicit takeover after reviewing the reported conflicts
    powershell -File scripts\start-openrgb2.ps1 -TakeOver
#>
[CmdletBinding()]
param(
    # Install root. Default: parent of this script's dir (scripts/ sits
    # at the install root in the staged package).
    [string]$InstallDir,

    # Optional scene file passed to the engine's --scene.
    [string]$Scene,

    # Skip the last-scene restore: without -Scene the engine
    # otherwise re-applies profiles\local\last-scene.json (the
    # most recent non-empty applied scene). Pass this to start
    # with NO scene at all.
    [switch]$NoSceneRestore,

    # Start backend + engine but not the UI.
    [switch]$NoUI,

    # EXPLICIT ownership takeover: required before startup proceeds
    # while competing RGB owners are running. Competitors are left
    # untouched -- the operator closes them; we never kill. One
    # exception: a foreign OpenRGB.exe still blocks the backend spawn
    # (the pinned binary's own single-instance prompt could terminate
    # it -- this script refuses rather than trigger that prompt).
    [switch]$TakeOver,

    # Run the ownership scan, print findings, exit (3 = conflicts).
    [switch]$ScanOnly,

    # Re-launch this script elevated (UAC prompt = the user-visible
    # step). Needed when the backend's hardware access requires
    # admin (e.g. the optional PawnIO driver path). Children of an
    # already-elevated launch inherit elevation.
    [switch]$Elevated,

    # GUI shortcut only: keep the engine's console window hidden.
    [switch]$HideWindows,

    # Bound on waiting for the backend's SDK listener.
    [int]$BackendWaitSeconds = 90,

    # Offline-test seams: classify THIS process list instead of the
    # live table (entries @{pid=..; exe=..}), and pretend the managed
    # port's listener is this PID (default -1 = real lookup). Same
    # role as EngineHost's injected seams -- never used by the
    # shipped flow.
    [object[]]$ProcessList,
    [int]$ListenerPidOverride = -1
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$ManagedPort = 6744

#--------------------------------------------------------------------#
# Ownership scan -- mirrors openrgb2::ClassifyOwners exactly. The     #
# packaging suite MECHANICALLY holds parity: T1 in                    #
# tests/packaging/package_lifecycle_tests.ps1 extracts the rule       #
# tokens and owner labels straight out of                             #
# src/engine/OwnershipScan.cpp and compares both sets to this         #
# mirror -- drift fails the test, it cannot silently diverge.         #
#--------------------------------------------------------------------#
function Get-OwnerFindings {
    param(
        [object[]]$Processes,   # @{pid=<int>; exe="<image name>"}
        [int]$SelfPid,
        [int]$BackendPid
    )
    $out = @()
    foreach($p in $Processes) {
        $procId = [int]$p.pid
        if($procId -eq 0 -or $procId -eq $SelfPid) { continue }
        $exe     = Split-Path -Leaf ([string]$p.exe)
        $name    = $exe.ToLowerInvariant()
        $compact = ($exe -replace '[^a-zA-Z0-9]','').ToLowerInvariant()

        $f = [ordered]@{ conflict=$false; owner=""; key=""; exe=$exe; pid=$procId; detail="" }

        if($name -eq "openrgb.exe") {
            if($procId -eq $BackendPid -and $BackendPid -ne 0) {
                $f.owner  = "OpenRGB (managed backend)"; $f.key = "openrgb"
                $f.detail = "owns the SDK listener -- the expected sole hardware owner"
            } else {
                $f.conflict = $true
                $f.owner    = "OpenRGB (other instance)"; $f.key = "openrgb"
                $f.detail   = "a second OpenRGB may own RGB hardware or host the legacy Studio/Effects producers -- report only, never killed"
            }
            $out += [pscustomobject]$f
        }
        elseif($compact.Contains("lconnect") -or $compact.Contains("lianli")) {
            $f.conflict = $true; $f.owner = "L-Connect"; $f.key = "lianli"
            $f.detail   = "competing RGB owner (Lian Li)"
            $out += [pscustomobject]$f
        }
        elseif($name -eq "gcc.exe" -or $compact.Contains("gigabytecontrolcenter") -or
               $compact.Contains("rgbfusion") -or $compact.Contains("aorusengine")) {
            $f.conflict = $true; $f.owner = "Gigabyte Control Center / RGB Fusion"; $f.key = "gigabyte"
            $f.detail   = "competing RGB owner (motherboard/GPU)"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("synapse") -or
               ($name.Contains("razer") -and
                ($name.Contains("central") -or $name.Contains("cortex")))) {
            $f.conflict = $true; $f.owner = "Razer Synapse"; $f.key = "razer"
            $f.detail   = "competing RGB owner (Razer peripherals)"
            $out += [pscustomobject]$f
        }
        elseif($compact.Contains("lamparray")) {
            $f.conflict = $true; $f.owner = "LampArray service (Dynamic Lighting)"; $f.key = "lamparray"
            $f.detail   = "Windows Dynamic Lighting bridge -- repaints its devices (e.g. Logitech keyboards) over the backend"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("lghub") -or $name -eq "lcore.exe" -or
               $compact.Contains("logioptions")) {
            $f.conflict = $true; $f.owner = "Logitech G Hub / Options"; $f.key = "logitech"
            $f.detail   = "competing RGB owner (Logitech peripherals)"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("icue") -or $name -eq "corsair.service.exe") {
            $f.conflict = $true; $f.owner = "Corsair iCUE"; $f.key = "corsair"
            $f.detail   = "competing RGB owner (Corsair)"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("signalrgb")) {
            $f.conflict = $true; $f.owner = "SignalRGB"; $f.key = "signalrgb"
            $f.detail   = "competing RGB owner"
            $out += [pscustomobject]$f
        }
        elseif($name -eq "fancontrol.exe") {
            $f.owner  = "FanControl"; $f.key = "fancontrol"
            $f.detail = "cooling owner -- expected; must keep running untouched"
            $out += [pscustomobject]$f
        }
    }
    return ,$out
}

# The PID listening on the managed SDK port (0 = nothing). Matches
# the engine's FindTcpListenerPid; netstat fallback for hosts without
# the NetTCPIP module.
function Find-ListenerPid {
    param([int]$Port)
    try {
        $row = Get-NetTCPConnection -LocalPort $Port -State Listen `
                   -ErrorAction Stop | Select-Object -First 1
        if($row) { return [int]$row.OwningProcess }
    } catch {
        $line = netstat -ano -p tcp |
            Select-String "LISTENING" |
            Where-Object { $_ -match "[:.]$Port\s" } |
            Select-Object -First 1
        if($line -and ($line -match '(\d+)\s*$')) { return [int]$Matches[1] }
    }
    return 0
}

function Write-Findings([object[]]$Findings) {
    foreach($f in $Findings) {
        $tag = if($f.conflict) { "CONFLICT" } else { "notice  " }
        # R-10: the trailing key lets the UI's takeover dialog add the
        # matching hint without re-classifying the label. A finding
        # built elsewhere without a key prints no tag.
        $key = if($f.PSObject.Properties['key']) { [string]$f.key } else { "" }
        $tagKey = if($key) { " [key: $key]" } else { "" }
        Write-Host ("  ownership {0}: {1} (pid {2}, {3}) -- {4}{5}" -f `
            $tag, $f.owner, $f.pid, $f.exe, $f.detail, $tagKey)
    }
}

# The takeover gate. $true = startup may proceed. Pure function over
# findings + the explicit consent flag -- the refusal path is what the
# offline suite drives; there is no kill path to stub out.
function Assert-TakeoverConsent {
    param([object[]]$Findings, [bool]$Consent)
    $conflicts = @($Findings | Where-Object { $_.conflict })
    if($conflicts.Count -eq 0) { return $true }
    if($Consent) {
        Write-Host ("TAKEOVER explicitly confirmed -- proceeding with " +
            "$($conflicts.Count) competing owner(s) LEFT RUNNING " +
            "(close them yourself for hardware access; OpenRGB2 " +
            "never kills competitors):") -ForegroundColor Yellow
        foreach($c in $conflicts) {
            Write-Host "  competitor left running: $($c.owner) (pid $($c.pid))" -ForegroundColor Yellow
        }
        Write-Host ("  exception: a foreign OpenRGB.exe still blocks the backend " +
            "spawn -- the pinned binary's own single-instance prompt could " +
            "TERMINATE it, and that prompt is never triggered from here") `
            -ForegroundColor Yellow
        return $true
    }
    Write-Host "" 
    Write-Host "OWNERSHIP CONFLICT -- OpenRGB2 will not take over silently." -ForegroundColor Red
    Write-Host "Competing RGB owners are running (reported above). To proceed:" -ForegroundColor Red
    Write-Host "  * close the competing application yourself (recommended), or"
    Write-Host "  * re-run with -TakeOver to explicitly take over -- competitors stay running."
    return $false
}

function Get-ProcessImagePath([int]$ProcId) {
    try { return (Get-Process -Id $ProcId -ErrorAction Stop).Path }
    catch { return $null }
}

# True when the PID is a LIVE process whose image path cannot be read
# from this shell (elevated, or another user's) -- unverifiable, as
# opposed to gone (no process) or readable.
function Test-UnverifiableProcess([int]$ProcId) {
    if($ProcId -le 0) { return $false }
    $p = $null
    try { $p = Get-Process -Id $ProcId -ErrorAction Stop } catch { return $false }
    $img = $null
    try { $img = $p.Path } catch { $img = $null }
    return (-not $img)
}

# Same-named managed-image PIDs whose image path is unreadable. With
# the -ProcessList seam only the listed entries are considered (the
# offline suite must not depend on the host's live process table).
function Find-UnverifiableManaged {
    param([object[]]$ProcessList, [int]$ListenerPid)
    $names = @("openrgb.exe", "openrgb2-engine.exe")
    $ids = @()
    if($ProcessList) {
        foreach($p in $ProcessList) {
            $leaf = (Split-Path -Leaf ([string]$p.exe)).ToLowerInvariant()
            if($names -contains $leaf) { $ids += [int]$p.pid }
        }
    } else {
        $ids += @(Get-Process -Name "OpenRGB","openrgb2-engine" `
                    -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    }
    if($ListenerPid -gt 0) { $ids += $ListenerPid }
    return ,@($ids | Sort-Object -Unique | Where-Object { Test-UnverifiableProcess $_ })
}

# Keep the desktop UI's logs bounded: past $MaxBytes the log is
# RENAMED to <log>.1 (replacing the previous .1) -- never truncated in
# place, so a reader that remembers an offset into the file is never
# handed a shorter file under the same name. Only when nobody holds it:
# an exclusive open must succeed first (a writer or reader that has it
# open -- e.g. this script's own redirected stdout, or a transcript --
# means rotation is skipped this time and retried next start). Best
# effort -- a log that cannot be rotated is left alone, never lost.
function Invoke-LogRotation([string]$Path, [long]$MaxBytes = 1MB) {
    try {
        $fi = Get-Item -LiteralPath $Path -ErrorAction Stop
        if($fi.Length -le $MaxBytes) { return }
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $fs.Dispose()
        $old = "$Path.1"
        if(Test-Path -LiteralPath $old) {
            Remove-Item -LiteralPath $old -Force -ErrorAction Stop
        }
        [IO.File]::Move($Path, $old)
    } catch { }
}

#--------------------------------------------------------------------#
# Startup                                                            #
#--------------------------------------------------------------------#
function Invoke-ManagedStart {
    param(
        [string]$InstallDir,
        [string]$Scene,
        [bool]$NoSceneRestore,
        [bool]$NoUI,
        [bool]$TakeOver,
        [bool]$ScanOnly,
        [bool]$Elevated,
        [bool]$HideWindows,
        [int]$BackendWaitSeconds,
        [object[]]$ProcessList,
        [int]$ListenerPidOverride
    )

    if(-not $InstallDir) { $InstallDir = Split-Path -Parent $PSScriptRoot }
    $InstallDir = (Resolve-Path $InstallDir).Path

    $UiExe      = Join-Path $InstallDir "openrgb2-ui.exe"
    $EngineExe  = Join-Path $InstallDir "openrgb2-engine.exe"
    $BackendExe = Join-Path $InstallDir "backend\OpenRGB.exe"
    $StateDir   = Join-Path $InstallDir "state"
    $BackendCfg = Join-Path $StateDir   "backend"
    $RunFile    = Join-Path $StateDir   "openrgb2.run.json"

    foreach($log in @("startup.log", "shutdown.log")) {
        Invoke-LogRotation (Join-Path $StateDir $log)
    }

    foreach($req in @($EngineExe, $BackendExe)) {
        if(-not (Test-Path $req)) {
            Write-Host "ERROR: install is missing $req" -ForegroundColor Red
            return 1
        }
    }

    # Optional self-elevation -- UAC prompt is the user-visible step.
    if($Elevated) {
        $elevArgs = @("-NoProfile","-ExecutionPolicy","Bypass","-File",
                      "`"$PSCommandPath`"",
                      "-InstallDir","`"$InstallDir`"")
        if($Scene)  { $elevArgs += @("-Scene","`"$Scene`"") }
        if($NoSceneRestore) { $elevArgs += "-NoSceneRestore" }
        if($NoUI)   { $elevArgs += "-NoUI" }
        if($TakeOver){ $elevArgs += "-TakeOver" }
        if($ScanOnly){ $elevArgs += "-ScanOnly" }
        if($HideWindows) { $elevArgs += "-HideWindows" }
        $elevArgs += @("-BackendWaitSeconds", "$BackendWaitSeconds")
        # Wait for the elevated copy and return ITS exit code (audit
        # P-14: -ScanOnly was dropped -- the elevated copy started the
        # whole stack -- and 0 came back before anything had run).
        # WaitForExit, not Start-Process -Wait: -Wait also waits for
        # the backend/engine the elevated copy leaves running.
        Write-Host "re-launching elevated (UAC prompt)..."
        try {
            $ep = Start-Process powershell.exe -Verb RunAs -PassThru `
                -ArgumentList ($elevArgs -join " ")
        } catch {
            Write-Host "ERROR: elevation declined or unavailable: $($_.Exception.Message)" -ForegroundColor Red
            return 1
        }
        $ep.WaitForExit()
        if($ep.ExitCode -ne 0) {
            Write-Host ("elevated start returned $($ep.ExitCode) -- for the " +
                "full output re-run in an elevated PowerShell: " +
                "scripts\start-openrgb2.ps1") -ForegroundColor Yellow
        }
        return $ep.ExitCode
    }

    Write-Host "==> OpenRGB2 managed startup ($InstallDir)" -ForegroundColor Cyan

    #---- 1. ownership scan (report only -- never kills) --------------
    $procs =
        if($ProcessList) { $ProcessList }
        else {
            @(Get-Process | ForEach-Object {
                [pscustomobject]@{ pid = $_.Id; exe = ($_.ProcessName + ".exe") }
            })
        }
    $backendPid = if($ListenerPidOverride -ge 0) { $ListenerPidOverride }
                  else { Find-ListenerPid $ManagedPort }
    $findings   = Get-OwnerFindings -Processes $procs -SelfPid $PID `
                      -BackendPid $backendPid

    # Reclassify OUR pinned backend: an OpenRGB.exe at THIS install's
    # backend path is never foreign -- when it has not bound the port
    # yet the listener check above missed it, so it lands in "other
    # instance". It is ours: a notice, not a conflict. (A foreign
    # OpenRGB.exe keeps its conflict -- and later REFUSES the backend
    # spawn so the pinned binary's own kill-capable single-instance
    # prompt is never triggered.)
    foreach($fnd in @($findings | Where-Object {
            $_.owner -eq "OpenRGB (other instance)" })) {
        $img = Get-ProcessImagePath ([int]$fnd.pid)
        if($img -and $img.Equals($BackendExe,
                [StringComparison]::OrdinalIgnoreCase)) {
            $fnd.owner    = "OpenRGB (managed backend)"
            $fnd.conflict = $false
            $fnd.detail   = "still starting -- our pinned backend, port not bound yet"
        }
    }

    if($backendPid -ne 0) {
        $img = Get-ProcessImagePath $backendPid
        if($img -and
           $img.Equals($BackendExe, [StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "  managed backend already listening on :$ManagedPort (pid $backendPid) -- will attach"
        } else {
            # A foreign process holds the managed endpoint: our backend
            # cannot bind. Surface it as a conflict finding so the same
            # gate applies -- with -TakeOver the engine attaches to the
            # foreign listener (it reports protocol/ownership honestly);
            # without it we refuse. We never kill the squatter.
            $imgDesc = if($img) { $img } else { "image path unreadable" }
            $findings += [pscustomobject][ordered]@{
                conflict = $true
                owner    = "foreign listener on :$ManagedPort"
                key      = "foreign_listener"
                exe      = $imgDesc
                pid      = $backendPid
                detail   = "the managed SDK port is held by a process that is not the pinned backend -- report only, never killed"
            }
        }
    }

    if($findings.Count -eq 0) {
        Write-Host "  ownership scan: no competing RGB owners detected"
    } else {
        Write-Findings $findings
    }

    # An unreadable image path is UNVERIFIABLE, not foreign: from a
    # non-elevated shell our own elevated backend/engine look exactly
    # like this. Refuse (even with -TakeOver) rather than mistake our
    # backend for a foreign listener or spawn a second engine and
    # overwrite the run file.
    $unverifiable = Find-UnverifiableManaged -ProcessList $ProcessList `
                        -ListenerPid $backendPid
    if($unverifiable.Count -gt 0) {
        Write-Host ""
        Write-Host ("UNVERIFIABLE: OpenRGB.exe / openrgb2-engine.exe / the " +
            ":$ManagedPort listener (pid $($unverifiable -join ', ')) run with " +
            "an image path this shell cannot read -- most likely an ELEVATED " +
            "OpenRGB2 stack. It cannot be proven ours or foreign, so nothing " +
            "is started. Re-run from an elevated PowerShell (or with " +
            "-Elevated); to restart it, stop it first: " +
            "scripts\stop-openrgb2.ps1 -Elevated.") -ForegroundColor Red
        return 4
    }

    if($ScanOnly) {
        if(@($findings | Where-Object { $_.conflict }).Count -gt 0) { return 3 }
        return 0
    }

    #---- 2. explicit takeover gate -----------------------------------
    if(-not (Assert-TakeoverConsent -Findings $findings -Consent $TakeOver)) {
        return 3
    }

    $foreignPortOwner =
        ($backendPid -ne 0) -and
        -not ((Get-ProcessImagePath $backendPid) -eq $BackendExe)

    #---- 3. backend ---------------------------------------------------
    # The pinned backend carries its OWN single-instance guard
    # (vendored startup\main_Windows.cpp): launched while a foreign
    # OpenRGB.exe lives it pops a Yes/No dialog whose Yes hard-kills
    # that instance via TerminateProcess (and its server probe checks
    # the legacy default port 6742, not the managed $ManagedPort). The
    # vendored cli has NO flag to skip the guard, so the launcher never
    # puts the backend in that position: a foreign OpenRGB.exe REFUSES
    # startup (the operator closes it -- still user-driven, never a
    # kill), and an OpenRGB.exe at OUR pinned path is a still-starting
    # managed backend -- waited on, never double-spawned (a second copy
    # would hit the same guard and offer to kill OUR backend).
    $startedBackend = $false
    $backendProcId  = 0
    $listenNow      = { if($ListenerPidOverride -ge 0) { $ListenerPidOverride }
                        else { Find-ListenerPid $ManagedPort } }
    if($backendPid -eq 0) {
        # Scan already split the buckets by image path: "OpenRGB (other
        # instance)" is FOREIGN (unreadable path counts as foreign --
        # conservative), "OpenRGB (managed backend)" is OUR pinned
        # backend still starting.
        $foreignOpenRgb = @($findings | Where-Object {
                $_.owner -eq "OpenRGB (other instance)" })
        $ownStarting    = @($findings | Where-Object {
                $_.owner -eq "OpenRGB (managed backend)" })
        if($foreignOpenRgb.Count -gt 0) {
            Write-Host ""
            Write-Host "BACKEND LAUNCH REFUSED -- a foreign OpenRGB.exe is running:" -ForegroundColor Red
            foreach($fnd in $foreignOpenRgb) {
                $fimg = Get-ProcessImagePath ([int]$fnd.pid)
                $imgDesc = if($fimg) { $fimg } else { "image path unreadable" }
                Write-Host "  pid $($fnd.pid): $imgDesc" -ForegroundColor Red
            }
            Write-Host "  Launching the pinned backend now would open its OWN single-instance prompt, which can TERMINATE the foreign instance (TerminateProcess)." -ForegroundColor Red
            Write-Host "  OpenRGB2 never triggers that prompt -- close the foreign OpenRGB.exe yourself, then re-run this script." -ForegroundColor Red
            return 3
        }
        if($ownStarting.Count -gt 0) {
            $backendProcId = [int]$ownStarting[0].pid
            Write-Host "==> managed backend already starting (pid $backendProcId) -- waiting for :$ManagedPort (never spawning a second copy)" -ForegroundColor Cyan
            $deadline = (Get-Date).AddSeconds($BackendWaitSeconds)
            while((Get-Date) -lt $deadline -and (& $listenNow) -eq 0) {
                Start-Sleep -Milliseconds 500
            }
            $backendPid = (& $listenNow)
            if($backendPid -eq 0) {
                Write-Host "WARNING: starting backend has not bound :$ManagedPort within ${BackendWaitSeconds}s -- starting the engine anyway (it re-probes the backend on a bounded schedule)" -ForegroundColor Yellow
            } else {
                Write-Host "  backend listening on :$ManagedPort (pid $backendPid)"
            }
        } else {
            New-Item -ItemType Directory -Force -Path $BackendCfg | Out-Null
            Write-Host "==> starting pinned backend (config: state\backend)" -ForegroundColor Cyan
            $backendStart = @{
                FilePath = $BackendExe
                PassThru = $true
                WorkingDirectory = $BackendCfg
                ArgumentList = @("--server","--server-host","127.0.0.1",
                                 "--server-port","$ManagedPort",
                                 "--noautoconnect","--localconfig")
            }
            if($HideWindows) { $backendStart.WindowStyle = 'Hidden' }
            $bp = Start-Process @backendStart
            $backendProcId  = $bp.Id
            $startedBackend = $true
            $deadline = (Get-Date).AddSeconds($BackendWaitSeconds)
            while((Get-Date) -lt $deadline -and (& $listenNow) -eq 0) {
                Start-Sleep -Milliseconds 500
                $bp.Refresh()
                if($bp.HasExited) {
                    Write-Host "ERROR: backend exited during startup (code $($bp.ExitCode))" -ForegroundColor Red
                    return 1
                }
            }
            $backendPid = (& $listenNow)
            if($backendPid -eq 0) {
                Write-Host "WARNING: backend has not bound :$ManagedPort within ${BackendWaitSeconds}s -- starting the engine anyway (it re-probes the backend on a bounded schedule)" -ForegroundColor Yellow
            } else {
                Write-Host "  backend listening on :$ManagedPort (pid $backendPid)"
            }
        }
    } elseif($foreignPortOwner) {
        Write-Host "==> not spawning backend: :$ManagedPort held by foreign process (takeover attaches the engine to it)" -ForegroundColor Yellow
    } else {
        $backendProcId = $backendPid
        Write-Host "==> reusing running managed backend (pid $backendPid)" -ForegroundColor Cyan
    }

    #---- 4. engine ----------------------------------------------------
    $engineArgs = @("--sdk",
                    "--backend-config", $BackendCfg,
                    "--backend-exe", $BackendExe,
                    "--devices-dir", (Join-Path $InstallDir "presets\devices"),
                    "--effects-dir", (Join-Path $InstallDir "presets\effects"),
                    "--state", (Join-Path $InstallDir "profiles\local\engine-state.json"),
                    "--workspace-root", (Join-Path $InstallDir "profiles\local\workspaces"))
    if($Scene) { $engineArgs += @("--scene", $Scene) }
    if($NoSceneRestore) { $engineArgs += "--no-scene-restore" }
    # -ArgumentList joins with spaces -- quote elements that need it
    # (install paths under e.g. "C:\Program Files" must survive).
    $engineArgLine = ($engineArgs | ForEach-Object {
        if($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '

    $runningEngine = Get-Process -Name "openrgb2-engine" -ErrorAction SilentlyContinue |
        Where-Object { try { $_.Path -eq $EngineExe } catch { $false } } |
        Select-Object -First 1
    $engineProcId = 0
    if($runningEngine) {
        $engineProcId = $runningEngine.Id
        Write-Host "==> engine already running (pid $engineProcId) -- not starting a second instance" -ForegroundColor Yellow
    } else {
        Write-Host "==> starting engine (attaches to :$ManagedPort; state: profiles\local\engine-state.json)" -ForegroundColor Cyan
        $engineStart = @{
            FilePath = $EngineExe
            PassThru = $true
            WorkingDirectory = $InstallDir
            ArgumentList = $engineArgLine
        }
        if($HideWindows) { $engineStart.WindowStyle = 'Hidden' }
        $ep = Start-Process @engineStart
        $engineProcId = $ep.Id
        Start-Sleep -Milliseconds 500
        $ep.Refresh()
        if($ep.HasExited) {
            Write-Host "ERROR: engine exited during startup (code $($ep.ExitCode)) -- see its console output" -ForegroundColor Red
            # The backend THIS run spawned used to stay up with no
            # engine and no run file (audit P-14). Close it the
            # graceful way stop-openrgb2.ps1 does (its PID-scoped
            # shutdown event -- never a kill); a backend without the
            # event is recorded in the run file instead so the managed
            # stop can close it.
            if($startedBackend -and $backendProcId -ne 0) {
                $bkGone = $false
                try {
                    $ev = [System.Threading.EventWaitHandle]::OpenExisting(
                        "Local\OpenRGB2-Backend-Shutdown-$backendProcId")
                    $ev.Set() | Out-Null
                    $ev.Dispose()
                    $bkGone = $bp.WaitForExit(15000)
                } catch { $bkGone = $false }
                if($bkGone) {
                    Write-Host "  the backend this start launched (pid $backendProcId) was closed"
                } else {
                    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
                    [ordered]@{
                        install_dir     = $InstallDir
                        started_utc     = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
                        engine_pid      = 0
                        backend_pid     = $backendProcId
                        backend_spawned = $true
                        foreign_port    = $foreignPortOwner
                        takeover        = $TakeOver
                        engine_exe      = $EngineExe
                        backend_exe     = $BackendExe
                    } | ConvertTo-Json | Set-Content -Encoding UTF8 $RunFile
                    Write-Host ("  the backend this start launched (pid $backendProcId) " +
                        "is still running -- recorded in state\openrgb2.run.json; " +
                        "stop it with scripts\stop-openrgb2.ps1") -ForegroundColor Yellow
                }
            }
            return 1
        }
    }

    #---- run state (drives the ordered stop) -------------------------
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    [ordered]@{
        install_dir       = $InstallDir
        started_utc       = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        engine_pid        = $engineProcId
        backend_pid       = if($backendProcId -ne 0) { $backendProcId } else { $backendPid }
        backend_spawned   = $startedBackend
        foreign_port      = $foreignPortOwner
        takeover          = $TakeOver
        engine_exe        = $EngineExe
        backend_exe       = $BackendExe
    } | ConvertTo-Json | Set-Content -Encoding UTF8 $RunFile
    Write-Host "  run state -> state\openrgb2.run.json"

    #---- 5. UI ---------------------------------------------------------
    if(-not $NoUI -and (Test-Path $UiExe)) {
        Write-Host "==> starting UI (engine keeps running if the UI closes)" -ForegroundColor Cyan
        Start-Process -FilePath $UiExe -WorkingDirectory $InstallDir `
            -ArgumentList '--unmanaged' | Out-Null
    }

    Write-Host "OpenRGB2 started. Stop with: scripts\stop-openrgb2.ps1" -ForegroundColor Green
    return 0
}

# Dot-source guard: sourcing this file defines the functions only.
if($MyInvocation.InvocationName -ne '.') {
    # An elevated run (the desktop UI and the sign-in task run this
    # script elevated) must not auto-load modules -- NetTCPIP for
    # Get-NetTCPConnection -- from the user's own, user-writable
    # module path.
    try {
        $isAdmin = (New-Object Security.Principal.WindowsPrincipal(
            [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { $isAdmin = $false }
    if($isAdmin) {
        $env:PSModulePath = (Join-Path $env:ProgramFiles "WindowsPowerShell\Modules") +
            ";" + (Join-Path $PSHOME "Modules")
    }
    exit (Invoke-ManagedStart `
        -InstallDir $InstallDir -Scene $Scene `
        -NoSceneRestore $NoSceneRestore -NoUI $NoUI `
        -TakeOver $TakeOver -ScanOnly $ScanOnly -Elevated $Elevated `
        -HideWindows $HideWindows `
        -BackendWaitSeconds $BackendWaitSeconds -ProcessList $ProcessList `
        -ListenerPidOverride $ListenerPidOverride)
}
