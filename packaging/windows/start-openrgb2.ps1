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
    idles at the 60 Hz scene clock until the UI applies a scene.

    Run state is recorded in state\openrgb2.run.json (PIDs + image
    paths) so stop-openrgb2.ps1 can order the shutdown and verify it
    only ever closes OUR processes.

    Exit codes: 0 started/attached, 1 fatal error (missing files),
    3 takeover refused (conflicts present, no -TakeOver).

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

    # Start backend + engine but not the UI.
    [switch]$NoUI,

    # EXPLICIT ownership takeover: required before startup proceeds
    # while competing RGB owners are running. Competitors are left
    # untouched -- the operator closes them; we never kill.
    [switch]$TakeOver,

    # Run the ownership scan, print findings, exit (3 = conflicts).
    [switch]$ScanOnly,

    # Re-launch this script elevated (UAC prompt = the user-visible
    # step). Needed when the backend's hardware access requires
    # admin (e.g. the optional PawnIO driver path). Children of an
    # already-elevated launch inherit elevation.
    [switch]$Elevated,

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
# Ownership scan -- mirrors openrgb2::ClassifyOwners exactly. Keep    #
# the name rules in sync with src/engine/OwnershipScan.cpp: any     #
# drift means the launcher and the engine report different owners.  #
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

        $f = [ordered]@{ conflict=$false; owner=""; exe=$exe; pid=$procId; detail="" }

        if($name -eq "openrgb.exe") {
            if($procId -eq $BackendPid -and $BackendPid -ne 0) {
                $f.owner  = "OpenRGB (managed backend)"
                $f.detail = "owns the SDK listener -- the expected sole hardware owner"
            } else {
                $f.conflict = $true
                $f.owner    = "OpenRGB (other instance)"
                $f.detail   = "a second OpenRGB may own RGB hardware or host the legacy Studio/Effects producers -- report only, never killed"
            }
            $out += [pscustomobject]$f
        }
        elseif($compact.Contains("lconnect") -or $compact.Contains("lianli")) {
            $f.conflict = $true; $f.owner = "L-Connect"
            $f.detail   = "competing RGB owner (Lian Li)"
            $out += [pscustomobject]$f
        }
        elseif($name -eq "gcc.exe" -or $compact.Contains("gigabytecontrolcenter") -or
               $compact.Contains("rgbfusion") -or $compact.Contains("aorusengine")) {
            $f.conflict = $true; $f.owner = "Gigabyte Control Center / RGB Fusion"
            $f.detail   = "competing RGB owner (motherboard/GPU)"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("synapse") -or
               ($name.Contains("razer") -and
                ($name.Contains("central") -or $name.Contains("cortex")))) {
            $f.conflict = $true; $f.owner = "Razer Synapse"
            $f.detail   = "competing RGB owner (Razer peripherals)"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("lghub") -or $name -eq "lcore.exe" -or
               $compact.Contains("logioptions")) {
            $f.conflict = $true; $f.owner = "Logitech G Hub / Options"
            $f.detail   = "competing RGB owner (Logitech peripherals)"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("icue") -or $name -eq "corsair.service.exe") {
            $f.conflict = $true; $f.owner = "Corsair iCUE"
            $f.detail   = "competing RGB owner (Corsair)"
            $out += [pscustomobject]$f
        }
        elseif($name.Contains("signalrgb")) {
            $f.conflict = $true; $f.owner = "SignalRGB"
            $f.detail   = "competing RGB owner"
            $out += [pscustomobject]$f
        }
        elseif($name -eq "fancontrol.exe") {
            $f.owner  = "FanControl"
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
        Write-Host ("  ownership {0}: {1} (pid {2}, {3}) -- {4}" -f `
            $tag, $f.owner, $f.pid, $f.exe, $f.detail)
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

#--------------------------------------------------------------------#
# Startup                                                            #
#--------------------------------------------------------------------#
function Invoke-ManagedStart {
    param(
        [string]$InstallDir,
        [string]$Scene,
        [bool]$NoUI,
        [bool]$TakeOver,
        [bool]$ScanOnly,
        [bool]$Elevated,
        [int]$BackendWaitSeconds,
        [object[]]$ProcessList,
        [int]$ListenerPidOverride
    )

    if(-not $InstallDir) { $InstallDir = Split-Path -Parent $PSScriptRoot }
    $InstallDir = (Resolve-Path $InstallDir).Path

    $UiExe      = Join-Path $InstallDir "openrgb2-ui.exe"
    $EngineExe  = Join-Path $InstallDir "openrgb2-engine.exe"
    $CtlExe     = Join-Path $InstallDir "openrgb2-ctl.exe"
    $BackendExe = Join-Path $InstallDir "backend\OpenRGB.exe"
    $StateDir   = Join-Path $InstallDir "state"
    $BackendCfg = Join-Path $StateDir   "backend"
    $RunFile    = Join-Path $StateDir   "openrgb2.run.json"

    foreach($req in @($EngineExe, $BackendExe)) {
        if(-not (Test-Path $req)) {
            Write-Host "ERROR: install is missing $req" -ForegroundColor Red
            return 1
        }
    }

    # Optional self-elevation -- UAC prompt is the user-visible step.
    if($Elevated) {
        $args = @("-NoProfile","-ExecutionPolicy","Bypass","-File",
                  "`"$PSCommandPath`"",
                  "-InstallDir","`"$InstallDir`"")
        if($Scene)  { $args += @("-Scene","`"$Scene`"") }
        if($NoUI)   { $args += "-NoUI" }
        if($TakeOver){ $args += "-TakeOver" }
        Write-Host "re-launching elevated (UAC prompt)..."
        Start-Process powershell.exe -Verb RunAs -ArgumentList ($args -join " ")
        return 0
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
    $startedBackend = $false
    $backendProcId  = 0
    if($backendPid -eq 0) {
        New-Item -ItemType Directory -Force -Path $BackendCfg | Out-Null
        Write-Host "==> starting pinned backend (config: state\backend)" -ForegroundColor Cyan
        $bp = Start-Process -FilePath $BackendExe -PassThru `
              -WorkingDirectory $BackendCfg `
              -ArgumentList "--server","--server-host","127.0.0.1",
                            "--server-port","$ManagedPort",
                            "--noautoconnect","--localconfig"
        $backendProcId  = $bp.Id
        $startedBackend = $true
        $deadline = (Get-Date).AddSeconds($BackendWaitSeconds)
        $listenNow  = { if($ListenerPidOverride -ge 0) { $ListenerPidOverride }
                        else { Find-ListenerPid $ManagedPort } }
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
    } elseif($foreignPortOwner) {
        Write-Host "==> not spawning backend: :$ManagedPort held by foreign process (takeover attaches the engine to it)" -ForegroundColor Yellow
    } else {
        $backendProcId = $backendPid
        Write-Host "==> reusing running managed backend (pid $backendPid)" -ForegroundColor Cyan
    }

    #---- 4. engine ----------------------------------------------------
    $engineArgs = @("--sdk",
                    "--backend-config", $BackendCfg,
                    "--devices-dir", (Join-Path $InstallDir "presets\devices"),
                    "--effects-dir", (Join-Path $InstallDir "presets\effects"),
                    "--state", (Join-Path $InstallDir "profiles\local\engine-state.json"))
    if($Scene) { $engineArgs += @("--scene", $Scene) }
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
        $ep = Start-Process -FilePath $EngineExe -PassThru `
              -WorkingDirectory $InstallDir -ArgumentList $engineArgLine
        $engineProcId = $ep.Id
        Start-Sleep -Milliseconds 500
        $ep.Refresh()
        if($ep.HasExited) {
            Write-Host "ERROR: engine exited during startup (code $($ep.ExitCode)) -- see its console output" -ForegroundColor Red
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
        Start-Process -FilePath $UiExe -WorkingDirectory $InstallDir | Out-Null
    }

    Write-Host "OpenRGB2 started. Stop with: scripts\stop-openrgb2.ps1" -ForegroundColor Green
    return 0
}

# Dot-source guard: sourcing this file defines the functions only.
if($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-ManagedStart `
        -InstallDir $InstallDir -Scene $Scene -NoUI $NoUI `
        -TakeOver $TakeOver -ScanOnly $ScanOnly -Elevated $Elevated `
        -BackendWaitSeconds $BackendWaitSeconds -ProcessList $ProcessList `
        -ListenerPidOverride $ListenerPidOverride)
}
