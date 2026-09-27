<#
.SYNOPSIS
    Clean shutdown for a managed OpenRGB2 install (Task 6.2).

.DESCRIPTION
    Ordered teardown, reverse of start-openrgb2.ps1:

      1. engine -- sends the IPC QUIT verb (openrgb2-ctl.exe quit), the
         same "Quit All" hook the UI uses: the engine runs its ordered,
         bounded shutdown (power monitor -> IPC intake -> IPC server ->
         scene worker -> reactive inputs -> delivery lanes -> SDK
         transport). The script then waits for the process to exit
         within the bound.
      2. backend handoff -- the backend stays the sole hardware owner;
         it is closed AFTER the engine fully exits with a graceful
         CloseMainWindow (WM_CLOSE), never a hard kill -- so it can
         persist its own config (state\backend\OpenRGB.json).

    Only processes VERIFIED as ours are ever touched: a PID is only
    signaled when its image path matches this install's executables
    (the run file written by start-openrgb2.ps1 records both), or it
    is discovered by image path under this install. Foreign processes
    and PID-recycled strangers are never signaled, never killed.

    Profile preservation: the engine's durable profile
    (profiles\local\engine-state.json + .lkg) is written atomically on
    every change -- a clean shutdown neither truncates nor rewrites it;
    the script verifies the files still exist and reports their state.

    -Force is the explicit last resort: after the graceful bound
    expires it permits Stop-Process on VERIFIED OWN processes only --
    still never a foreign process.

    Exit codes: 0 clean (or nothing running), 1 partial (a process
    outlived its bound and -Force was not given).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\stop-openrgb2.ps1
#>
[CmdletBinding()]
param(
    # Install root. Default: parent of this script's dir.
    [string]$InstallDir,

    # Bound on each graceful wait (engine exit, backend exit).
    [int]$TimeoutSeconds = 45,

    # Permit Stop-Process on verified own processes after the
    # graceful bound expires. Never applies to foreign processes.
    [switch]$Force,

    # ctl override -- offline-test seam (the packaging suite injects a
    # stub that simulates the engine's ordered-exit response to QUIT).
    [string]$CtlExe
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

function Fail([string]$msg)   { Write-Host "ERROR: $msg" -ForegroundColor Red }
function Info([string]$msg)   { Write-Host "  $msg" }
function Step([string]$msg)   { Write-Host "==> $msg" -ForegroundColor Cyan }

# A PID is only ours when its image path resolves to THIS install's
# executable -- protects against PID reuse and foreign processes.
function Test-OwnProcess {
    param([int]$ProcId, [string]$ExpectedPath)
    if($ProcId -le 0) { return $false }
    try {
        $p = Get-Process -Id $ProcId -ErrorAction Stop
        return ($p.Path -and
                $p.Path.Equals($ExpectedPath,
                               [StringComparison]::OrdinalIgnoreCase))
    } catch { return $false }
}

# Discover a running managed process by image path (fallback when the
# run file is absent or its PID was recycled).
function Find-ByImagePath {
    param([string]$Name, [string]$ExpectedPath)
    return Get-Process -Name $Name -ErrorAction SilentlyContinue |
        Where-Object { try {
            $_.Path -and $_.Path.Equals($ExpectedPath,
                                        [StringComparison]::OrdinalIgnoreCase)
        } catch { $false } } | Select-Object -First 1
}

function Invoke-ManagedStop {
    param(
        [string]$InstallDir,
        [int]$TimeoutSeconds,
        [bool]$Force,
        [string]$CtlExe
    )

    if(-not $InstallDir) { $InstallDir = Split-Path -Parent $PSScriptRoot }
    $InstallDir = (Resolve-Path $InstallDir).Path

    $EngineExe  = Join-Path $InstallDir "openrgb2-engine.exe"
    $BackendExe = Join-Path $InstallDir "backend\OpenRGB.exe"
    if(-not $CtlExe) { $CtlExe = Join-Path $InstallDir "openrgb2-ctl.exe" }
    $StateDir   = Join-Path $InstallDir "state"
    $RunFile    = Join-Path $StateDir   "openrgb2.run.json"
    $ProfileDir = Join-Path $InstallDir "profiles\local"

    Step "OpenRGB2 managed shutdown ($InstallDir)"

    # Run state written by the launcher; absent = still able to stop
    # anything verifiably ours by image path.
    $run = $null
    if(Test-Path $RunFile) {
        try { $run = Get-Content $RunFile -Raw | ConvertFrom-Json }
        catch { Info "run file unreadable -- falling back to image-path discovery" }
    }

    $exitCode   = 0
    $engineGone = $false
    $backendGone= $false

    #---- 1. engine: IPC QUIT (ordered, bounded shutdown) -------------
    $enginePid = 0
    if($run -and (Test-OwnProcess ([int]$run.engine_pid) $EngineExe)) {
        $enginePid = [int]$run.engine_pid
    } elseif($p = Find-ByImagePath "openrgb2-engine" $EngineExe) {
        $enginePid = $p.Id
    }

    if($enginePid -eq 0) {
        Info "engine: not running"
        $engineGone = $true
    } else {
        Step "engine (pid $enginePid): QUIT via IPC -- ordered bounded shutdown"
        if(Test-Path $CtlExe) {
            & $CtlExe quit | Out-Null
        } else {
            Info "openrgb2-ctl not found -- waiting on the process directly"
        }
        $engProc = Get-Process -Id $enginePid -ErrorAction SilentlyContinue
        if($engProc -and -not $engProc.WaitForExit($TimeoutSeconds * 1000)) {
            if($Force -and (Test-OwnProcess $enginePid $EngineExe)) {
                Info "engine outlived the graceful bound -- Stop-Process (-Force, verified own process)"
                Stop-Process -Id $enginePid -Force
                $engProc.WaitForExit(5000) | Out-Null
            } else {
                Fail "engine (pid $enginePid) did not exit within ${TimeoutSeconds}s -- re-run with -Force to force-stop OUR engine, or investigate"
                $exitCode = 1
            }
        }
        $engineGone = -not (Get-Process -Id $enginePid -ErrorAction SilentlyContinue)
        if($engineGone) { Info "engine exited (ordered shutdown complete)" }
    }

    #---- 2. backend handoff: graceful close, never a kill ------------
    $backendPid = 0
    if($run -and (Test-OwnProcess ([int]$run.backend_pid) $BackendExe)) {
        $backendPid = [int]$run.backend_pid
    } elseif($p = Find-ByImagePath "OpenRGB" $BackendExe) {
        $backendPid = $p.Id
    }

    if($backendPid -eq 0) {
        Info "backend: not running (or not ours -- foreign processes are never touched)"
        $backendGone = $true
    } else {
        Step "backend (pid $backendPid): CloseMainWindow -- graceful handoff"
        $bkProc = Get-Process -Id $backendPid -ErrorAction SilentlyContinue
        $closed = $false
        try { $closed = $bkProc.CloseMainWindow() } catch { $closed = $false }
        if(-not $closed) {
            Info "no main window to close (minimized/headless) -- waiting for it to exit anyway"
        }
        if($bkProc -and -not $bkProc.WaitForExit($TimeoutSeconds * 1000)) {
            if($Force -and (Test-OwnProcess $backendPid $BackendExe)) {
                Info "backend outlived the graceful bound -- Stop-Process (-Force, verified own process)"
                Stop-Process -Id $backendPid -Force
                $bkProc.WaitForExit(5000) | Out-Null
            } else {
                Fail "backend (pid $backendPid) did not exit within ${TimeoutSeconds}s -- close its window/tray icon yourself, or re-run with -Force (verified own processes only)"
                $exitCode = 1
            }
        }
        $backendGone = -not (Get-Process -Id $backendPid -ErrorAction SilentlyContinue)
        if($backendGone) { Info "backend exited (config persisted under state\backend)" }
    }

    #---- 3. profile preservation report -------------------------------
    $state = Join-Path $ProfileDir "engine-state.json"
    if(Test-Path $state) {
        $hash = (Get-FileHash -Algorithm SHA256 $state).Hash.ToLower()
        Info "profile preserved: profiles\local\engine-state.json (sha256 $($hash.Substring(0,16))...)"
    } else {
        Info "no engine-state.json yet (fresh install -- nothing to preserve)"
    }
    foreach($sidecar in @("$state.lkg", "$state.rejected")) {
        if(Test-Path $sidecar) { Info "profile sidecar kept: $(Split-Path -Leaf $sidecar)" }
    }

    if(Test-Path $RunFile) { Remove-Item -Force $RunFile }

    if($engineGone -and $backendGone -and $exitCode -eq 0) {
        Write-Host "OpenRGB2 stopped cleanly." -ForegroundColor Green
    }
    return $exitCode
}

# Dot-source guard (packaging suite imports the functions).
if($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-ManagedStop -InstallDir $InstallDir `
        -TimeoutSeconds $TimeoutSeconds -Force $Force -CtlExe $CtlExe)
}
