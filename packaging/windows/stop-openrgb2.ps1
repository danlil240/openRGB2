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
         WM_CLOSE, never a hard kill -- so it can persist its own
         config (state\backend\OpenRGB.json). CloseMainWindow reaches
         only a MAIN window; a tray/headless backend has none, so the
         fallback posts WM_CLOSE to every top-level window the pid
         owns (taskkill without /F -- still graceful, still
         verified-own only).

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

    Elevation: a managed stack started elevated is UNVERIFIABLE from a
    non-elevated shell -- its children's image paths cannot be read,
    so ownership proof always fails. Such processes are reported as
    "running but unverifiable" (never "not running"), the run file is
    kept for the retry, and the script exits 1. -Elevated re-launches
    this script under UAC, same user-visible step as
    start-openrgb2.ps1.

    Exit codes: 0 clean (or nothing verifiably ours running),
    1 partial (a process outlived its bound without -Force, or
    same-named processes exist that cannot be verified -- hence
    cannot be touched).

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

    # Re-launch this script elevated (UAC prompt = the user-visible
    # step, parity with start-openrgb2.ps1). Needed to stop a stack
    # that was started elevated: only a same-integrity-or-higher
    # shell can read the children's image paths, so verification --
    # and therefore any signal -- requires elevation.
    [switch]$Elevated,

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

# Resolve the PID we may signal for one component: the run-file PID
# verified by image path, else image-path discovery. When resolution
# fails the result still explains WHY -- same-named processes may be
# running but unverifiable (image path unreadable from this
# shell's integrity level: elevated children or another user's
# processes) or foreign (image resolves to a different install).
# Both are listed so the report is honest; neither is ever touched.
function Resolve-OwnPid {
    param(
        [int]$RunFilePid,
        [string]$ImageName,
        [string]$ExpectedPath
    )
    if($RunFilePid -gt 0 -and
       (Test-OwnProcess $RunFilePid $ExpectedPath)) {
        return [pscustomobject]@{ Pid=$RunFilePid; Unverifiable=@(); Foreign=@() }
    }
    if($p = Find-ByImagePath $ImageName $ExpectedPath) {
        return [pscustomobject]@{ Pid=$p.Id; Unverifiable=@(); Foreign=@() }
    }
    $unverifiable = @()
    $foreign      = @()
    foreach($p in @(Get-Process -Name $ImageName -ErrorAction SilentlyContinue)) {
        $img = $null
        try { $img = $p.Path } catch { $img = $null }
        if($img) { $foreign      += [pscustomobject]@{ Id=$p.Id; Path=$img } }
        else     { $unverifiable += $p.Id }
    }
    return [pscustomobject]@{ Pid=0; Unverifiable=$unverifiable; Foreign=$foreign }
}

function Invoke-ManagedStop {
    param(
        [string]$InstallDir,
        [int]$TimeoutSeconds,
        [bool]$Force,
        [bool]$Elevated,
        [string]$CtlExe
    )

    if(-not $InstallDir) {
        $InstallDir = Split-Path -Parent $PSScriptRoot
        # Source-tree convenience (mirrors OpenRGB2.cmd): invoked from
        # packaging\windows the derived default lands on
        # ...\packaging, which is NOT an install root -- no exes, no
        # state\ -- so fall back to the newest staged package under
        # build\package\ and manage that install instead.
        if(-not (Test-Path (Join-Path $InstallDir "openrgb2-engine.exe"))) {
            $staged = Get-ChildItem (Join-Path $PSScriptRoot `
                        "..\..\build\package\OpenRGB2-*-win64") `
                        -Directory -ErrorAction SilentlyContinue |
                Where-Object {
                    Test-Path (Join-Path $_.FullName "openrgb2-engine.exe") } |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1
            if($staged) { $InstallDir = $staged.FullName }
        }
    }
    $InstallDir = (Resolve-Path $InstallDir).Path

    # Self-elevation (parity with start-openrgb2.ps1): an elevated
    # stack is invisible to ownership verification from a
    # non-elevated shell. -Elevated re-launches this script under UAC
    # with the resolved install dir; the child omits -Elevated. The
    # elevated child's own window shows its output; we wait and
    # propagate its exit code.
    if($Elevated) {
        $elevArgs = @("-NoProfile","-ExecutionPolicy","Bypass","-File",
                      "`"$PSCommandPath`"",
                      "-InstallDir","`"$InstallDir`"",
                      "-TimeoutSeconds","$TimeoutSeconds")
        if($Force)  { $elevArgs += "-Force" }
        if($CtlExe) { $elevArgs += @("-CtlExe","`"$CtlExe`"") }
        Write-Host "re-launching elevated (UAC prompt)..."
        try {
            $ep = Start-Process powershell.exe -Verb RunAs -Wait `
                -PassThru -ArgumentList ($elevArgs -join " ")
        } catch {
            Fail "elevation declined or unavailable: $($_.Exception.Message)"
            return 1
        }
        if($ep.ExitCode -ne 0) {
            Fail ("elevated stop returned $($ep.ExitCode) -- for the " +
                "full log re-run in an elevated PowerShell: " +
                "scripts\stop-openrgb2.ps1")
        }
        return $ep.ExitCode
    }

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
    $runEnginePid = 0
    if($run) { try { $runEnginePid = [int]$run.engine_pid } catch { } }
    $eng = Resolve-OwnPid -RunFilePid $runEnginePid `
        -ImageName "openrgb2-engine" -ExpectedPath $EngineExe
    $enginePid = [int]$eng.Pid

    if($enginePid -eq 0) {
        if($eng.Unverifiable.Count -gt 0) {
            Fail ("engine: same-named process running " +
                "(pid $($eng.Unverifiable -join ', ')) but its image " +
                "path is UNREADABLE from this shell -- the stack is " +
                "elevated (or owned by another user). Verification " +
                "never touches what it cannot prove is ours: re-run " +
                "from an elevated PowerShell, or re-run with -Elevated.")
            $exitCode = 1
        } elseif($eng.Foreign.Count -gt 0) {
            foreach($fp in $eng.Foreign) {
                Info ("engine: pid $($fp.Id) runs a DIFFERENT image " +
                    "($($fp.Path)) -- foreign, not touched")
            }
            Info "engine: nothing of ours running"
            $engineGone = $true
        } else {
            Info "engine: not running"
            $engineGone = $true
        }
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
    $runBackendPid = 0
    if($run) { try { $runBackendPid = [int]$run.backend_pid } catch { } }
    $bk = Resolve-OwnPid -RunFilePid $runBackendPid `
        -ImageName "OpenRGB" -ExpectedPath $BackendExe
    $backendPid = [int]$bk.Pid

    if($backendPid -eq 0) {
        if($bk.Unverifiable.Count -gt 0) {
            Fail ("backend: same-named process running " +
                "(pid $($bk.Unverifiable -join ', ')) but its image " +
                "path is UNREADABLE from this shell -- the stack is " +
                "elevated (or owned by another user). Verification " +
                "never touches what it cannot prove is ours: re-run " +
                "from an elevated PowerShell, or re-run with -Elevated.")
            $exitCode = 1
        } elseif($bk.Foreign.Count -gt 0) {
            foreach($fp in $bk.Foreign) {
                Info ("backend: pid $($fp.Id) runs a DIFFERENT image " +
                    "($($fp.Path)) -- foreign, not touched")
            }
            Info "backend: nothing of ours running"
            $backendGone = $true
        } else {
            Info "backend: not running"
            $backendGone = $true
        }
    } else {
        Step "backend (pid $backendPid): WM_CLOSE -- graceful handoff"
        $bkProc = Get-Process -Id $backendPid -ErrorAction SilentlyContinue
        $closed = $false
        try { $closed = $bkProc.CloseMainWindow() } catch { $closed = $false }
        if(-not $closed) {
            # Tray/headless backends have no MAIN window (handle 0) so
            # CloseMainWindow can't reach them. taskkill WITHOUT /F
            # posts the same WM_CLOSE to every top-level window the
            # pid owns -- graceful, and this pid is already verified
            # ours. It errors harmlessly when the process owns no
            # windows at all; the bounded wait below still applies.
            Info "no main window (tray/headless) -- posting WM_CLOSE to all its top-level windows"
            $null = & taskkill.exe /PID $backendPid 2>&1
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

    # Keep the run file on a non-clean exit: its recorded PIDs are
    # exactly what a retry (elevated, or with -Force) needs.
    if($exitCode -eq 0 -and (Test-Path $RunFile)) {
        Remove-Item -Force $RunFile
    }

    if($engineGone -and $backendGone -and $exitCode -eq 0) {
        Write-Host "OpenRGB2 stopped cleanly." -ForegroundColor Green
    }
    return $exitCode
}

# Dot-source guard (packaging suite imports the functions).
if($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-ManagedStop -InstallDir $InstallDir `
        -TimeoutSeconds $TimeoutSeconds -Force $Force `
        -Elevated ([bool]$Elevated) -CtlExe $CtlExe)
}
