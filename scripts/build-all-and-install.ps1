<#
.SYNOPSIS
    Build everything + install in place for OpenRGB2 (elevating only
    the steps that need it).

.DESCRIPTION
    One command from repo checkout to a working desktop icon:

      1. Frees the repo's build outputs: dev-run openrgb2/OpenRGB
         processes running from this repo (build\ outputs, staged
         packages, a vendor\OpenRGB backend started in place) are
         force-stopped when this shell can see their image path. If a
         repo executable is still locked afterwards -- an ELEVATED
         dev-run process, invisible to this shell -- an elevated
         "Sweep" step (UAC) stops it. Foreign installs are listed,
         never touched.
      2. Builds the pinned vendored backend via
         vendor\OpenRGB\scripts\build-windows.bat 6.8.3 2022 64
         (qmake + jom, incremental; -SkipBackend to skip) and stamps
         it with a hash of its sources (packaging\windows\
         backend-stamp.ps1). Step 3 refuses a backend whose stamp
         does not match, so -SkipBackend after a vendor change fails
         with "backend is stale" instead of deploying the old one.
      3. Runs packaging\windows\build-and-deploy.ps1 WITHOUT
         -InstallDir: CMake build of engine + UI + tools ->
         stage-release.ps1 (windeployqt + manifest).
         Steps 1-3 run as the invoking user, never elevated: compilers,
         qmake/jom, windeployqt and the staging copy have no need for
         admin rights.
      4. Elevated "Deploy" step (UAC): stops the installed managed
         stack via <InstallDir>\scripts\stop-openrgb2.ps1 -Force (UI ->
         engine IPC QUIT -> backend shutdown event -> verified
         Stop-Process fallback), force-stops anything still running
         from the install dir, then runs update-openrgb2.ps1 --
         manifest-verified, preserves profiles\local\ + state\,
         one-level .previous rollback -- which, being elevated, makes
         the install admin-only (state\ + profiles\local\ stay
         writable by the invoking user).
      5. Ensures a desktop shortcut targets <InstallDir>\openrgb2-ui.exe
         (creates OpenRGB2.lnk via create-desktop-shortcut.ps1 when
         none does), as the invoking user.

    Default InstallDir = %ProgramFiles%\OpenRGB2 -- an admin-only
    location, the only kind the optional Windows sign-in task accepts
    (a folder under your profile could be swapped out from above). The
    elevated Deploy step creates it on a first install.

    One install only: before anything is built or stopped, the script
    refuses when OpenRGB2 is already installed somewhere other than
    InstallDir (the known install roots, the desktop shortcut's target,
    the sign-in task's target). A second install starts with an empty
    profiles\local\ -- no workspace, so no layout and no effects -- and
    the desktop icon and sign-in task end up launching different
    builds. Re-run with -InstallDir <that install>, or remove it first.
    -AllowSecondInstall overrides the check.

    Started from an already-elevated shell, every step runs in that
    shell (no UAC prompts; the build then runs elevated too).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-all-and-install.ps1

.EXAMPLE
    scripts\build-all-and-install.cmd          (double-clickable)
#>
[CmdletBinding()]
param(
    # Install root the desktop icon launches. Default:
    # %ProgramFiles%\OpenRGB2 (admin-only; the sign-in task needs that).
    [string]$InstallDir = (Join-Path $env:ProgramFiles "OpenRGB2"),

    # Install even though OpenRGB2 is already installed elsewhere
    # (normally refused: one install location only).
    [switch]$AllowSecondInstall,

    # App build configuration (engine/UI/tools; staged inputs too).
    [string]$Configuration = "Release",

    # CMake build tree. Default: <repo>\build.
    [string]$BuildDir,

    # Re-run CMake configure even when the cache exists.
    [switch]$Reconfigure,

    # Skip the CMake app build -- stage + deploy from existing outputs.
    [switch]$SkipBuild,

    # Skip the vendored OpenRGB backend build (build-and-deploy still
    # verifies OpenRGB.exe exists and fails if it does not).
    [switch]$SkipBackend,

    # Authenticode-sign staged exes via Azure Trusted Signing
    # (passthrough to stage-release.ps1; needs OPENRGB2_TS_* env vars).
    [switch]$Sign,

    # Internal: the UAC-relaunched step to run ("Sweep" or "Deploy").
    [ValidateSet("", "Sweep", "Deploy")]
    [string]$ElevatedPhase = "",

    # Internal: SID of the invoking (non-elevated) user, who keeps
    # write access to state\ + profiles\local\.
    [string]$UserSid,

    # Don't hold a failed elevated window open at the end.
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$RepoRoot  = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$PkgDir    = Join-Path $RepoRoot "packaging\windows"
if(-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "build" }
# Absolute paths: the UAC child starts in another working directory.
$BuildDir   = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)
$InstallDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallDir)
$RelatedNames = @("openrgb2-ui","openrgb2-engine","openrgb2-ctl",
                  "OpenRGB","lianli_bench","skew_bench")

function Info([string]$msg) { Write-Host "  $msg" }
function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# Write-BackendStamp (audit P-7): the stamp build-and-deploy.ps1 checks.
. (Join-Path $PkgDir "backend-stamp.ps1")

# The lifecycle scripts end in `exit <code>` -- invoke each in a
# powershell -File subprocess so that exit lands in $LASTEXITCODE
# instead of terminating this wrapper's own session mid-run.
function Invoke-PsScript([string]$script, [string[]]$scriptArgs) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script @scriptArgs
    if($LASTEXITCODE -ne 0) { throw "$script failed (exit $LASTEXITCODE)" }
}

function Test-UnderRoot([string]$path, [string]$root) {
    if(-not $path) { return $false }
    return $path.StartsWith($root.TrimEnd('\') + '\',
                            [StringComparison]::OrdinalIgnoreCase)
}

function Test-IsElevated {
    $principal = New-Object Security.Principal.WindowsPrincipal(
                     [Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Force-stop related processes whose image lives under one of $roots.
# Unreadable image paths are listed, never touched.
function Stop-RelatedUnder([string[]]$roots) {
    $killed = 0; $hidden = 0
    foreach($p in @(Get-Process -Name $RelatedNames -ErrorAction SilentlyContinue)) {
        $img = $null
        try { $img = $p.Path } catch { $img = $null }
        $ours = $false
        if($img) {
            foreach($r in $roots) {
                if(Test-UnderRoot $img $r) { $ours = $true; break }
            }
        }
        if($ours) {
            Info "kill $($p.Name) (pid $($p.Id)) -- $img"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $killed++
        } elseif($img) {
            Info "foreign $($p.Name) (pid $($p.Id)) -- $img (not touched)"
        } else {
            Info "unverifiable $($p.Name) (pid $($p.Id)) -- image unreadable (not touched)"
            $hidden++
        }
    }
    if($killed -eq 0) { Info "nothing related running there" }
    return $hidden
}

# Repo executables that are loaded somewhere (exclusive-open fails):
# what an elevated dev-run process -- invisible to this shell -- holds.
function Find-LockedRepoExes {
    $dirs = @($BuildDir,
              (Join-Path $RepoRoot "vendor\OpenRGB\release"),
              (Join-Path $RepoRoot "vendor\OpenRGB\OpenRGB Windows 64-bit"))
    $locked = @()
    foreach($d in $dirs) {
        if(-not (Test-Path -LiteralPath $d)) { continue }
        foreach($exe in @(Get-ChildItem -LiteralPath $d -Recurse -File -Filter *.exe `
                              -ErrorAction SilentlyContinue)) {
            if($RelatedNames -notcontains $exe.BaseName) { continue }
            try {
                $fs = [IO.File]::Open($exe.FullName, [IO.FileMode]::Open,
                    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                $fs.Dispose()
            } catch [UnauthorizedAccessException] {
            } catch { $locked += $exe.FullName }
        }
    }
    return ,$locked
}

# Other OpenRGB2 installs on this machine: folders holding an
# openrgb2-ui.exe that are not $InstallDir and not inside the repo
# (build\package\ staging is not an install). Candidates: the known
# install roots, the desktop shortcuts' targets, the sign-in task's.
function Find-OtherInstalls {
    $candidates = @(
        (Join-Path $env:ProgramFiles "OpenRGB2"),
        (Join-Path $env:LOCALAPPDATA "Programs\OpenRGB2"))
    try {
        $wshell  = New-Object -ComObject WScript.Shell
        $desktop = [Environment]::GetFolderPath("Desktop")
        foreach($lnk in @(Get-ChildItem $desktop -Filter *.lnk `
                            -ErrorAction SilentlyContinue)) {
            $t = $wshell.CreateShortcut($lnk.FullName).TargetPath
            if($t -and (Split-Path -Leaf $t) -ieq "openrgb2-ui.exe") {
                $candidates += (Split-Path -Parent $t)
            }
        }
    } catch { }
    try {
        foreach($task in @(Get-ScheduledTask -TaskName "OpenRGB2-*" `
                              -ErrorAction SilentlyContinue)) {
            foreach($a in @($task.Actions)) {
                $t = [string]$a.Execute
                if($t -and (Split-Path -Leaf $t) -ieq "openrgb2-ui.exe") {
                    $candidates += (Split-Path -Parent $t)
                }
            }
        }
    } catch { }

    $self  = $InstallDir.TrimEnd('\')
    $found = @()
    foreach($c in $candidates) {
        $dir = $c.TrimEnd('\')
        if($dir -ieq $self) { continue }
        if(Test-UnderRoot $dir $RepoRoot) { continue }
        if($found -icontains $dir) { continue }
        if(Test-Path -LiteralPath (Join-Path $dir "openrgb2-ui.exe") -PathType Leaf) {
            $found += $dir
        }
    }
    return ,$found
}

function Assert-SingleInstall {
    $others = Find-OtherInstalls
    if($others.Count -eq 0) { return }
    foreach($o in $others) { Info "existing install: $o" }
    if($AllowSecondInstall) {
        Info "-AllowSecondInstall: installing to $InstallDir anyway"
        return
    }
    $first = $others[0]
    throw ("OpenRGB2 is already installed at $first -- refusing a second " +
           "install at $InstallDir (it would start with no workspace, and " +
           "the desktop icon and sign-in task would launch different builds). " +
           "Update that install instead: -InstallDir `"$first`". To move " +
           "it, stop it, copy its profiles\local\ to the new location, " +
           "delete the old folder, then re-run. -AllowSecondInstall overrides.")
}

function Get-InstallRoot {
    $r = Resolve-Path -LiteralPath $InstallDir -ErrorAction SilentlyContinue
    if($r -and -not (Test-UnderRoot $r.Path $RepoRoot)) { return $r.Path }
    return $null
}

#--------------------------------------------------------------------#
# Elevated steps                                                      #
#--------------------------------------------------------------------#
function Invoke-SweepPhase {
    Step "Stopping elevated processes running from the repo"
    $roots = @($RepoRoot)
    if($ir = Get-InstallRoot) { $roots += $ir }
    $null = Stop-RelatedUnder $roots
    return 0
}

function Invoke-DeployPhase {
    if(-not (Test-Path -LiteralPath $InstallDir)) {
        # First install under an admin-only parent (e.g.
        # %ProgramFiles%\OpenRGB2): only this elevated step can create it.
        New-Item -ItemType Directory -Force $InstallDir | Out-Null
        Info "created install dir (first install, elevated): $InstallDir"
    }
    $stopScript = Join-Path $InstallDir "scripts\stop-openrgb2.ps1"
    if(Test-Path $stopScript) {
        Step "Stopping installed stack ($InstallDir)"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $stopScript `
            -InstallDir $InstallDir -Force
        if($LASTEXITCODE -ne 0) {
            Info "stop-openrgb2 returned $LASTEXITCODE -- continuing to the sweep"
        }
    } else {
        Info "no installed stop script -- skipping managed shutdown"
    }
    if($ir = Get-InstallRoot) {
        Step "Stopping leftovers running from the install dir"
        $null = Stop-RelatedUnder @($ir)
    }

    $cmakeTop = Get-Content (Join-Path $RepoRoot "CMakeLists.txt") -Raw
    $version  = "0.0.0"
    if($cmakeTop -match "project\s*\(\s*OpenRGB2\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)") {
        $version = $Matches[1]
    }
    $outDir = Join-Path $BuildDir "package\OpenRGB2-$version-win64"
    Step "Applying package -> $InstallDir (elevated: install becomes admin-only)"
    $updArgs = @("-PackageDir", $outDir, "-InstallDir", $InstallDir)
    if($UserSid) { $updArgs += @("-UserSid", $UserSid) }
    Invoke-PsScript (Join-Path $PkgDir "update-openrgb2.ps1") $updArgs
    return 0
}

# Run an elevated step: in-process when this shell is already
# elevated, else one UAC-relaunched child (its own window).
function Invoke-Elevated([string]$phase) {
    if(Test-IsElevated) {
        if($phase -eq "Sweep") { return (Invoke-SweepPhase) }
        return (Invoke-DeployPhase)
    }
    Step "elevation required for the $phase step -- UAC prompt"
    $argList = @("-NoProfile","-ExecutionPolicy","Bypass","-File",
                 "`"$PSCommandPath`"",
                 "-ElevatedPhase", $phase,
                 "-InstallDir", "`"$InstallDir`"",
                 "-BuildDir", "`"$BuildDir`"",
                 "-UserSid", [Security.Principal.WindowsIdentity]::GetCurrent().User.Value)
    if($NoPause) { $argList += "-NoPause" }
    try {
        $proc = Start-Process -FilePath powershell.exe -Verb RunAs `
                    -Wait -PassThru -ArgumentList ($argList -join " ")
    } catch {
        throw "elevation declined or unavailable: $($_.Exception.Message)"
    }
    if($proc.ExitCode -ne 0) {
        throw "elevated $phase step failed (exit $($proc.ExitCode)) -- see its window"
    }
    return 0
}

function Invoke-Main {
    Step "OpenRGB2 build-all + install"
    Info "repo:    $RepoRoot"
    Info "install: $InstallDir"
    Info "config:  $Configuration"

    # 0. one install location only -- checked before anything is
    #    stopped or built.
    Assert-SingleInstall

    #----------------------------------------------------------------#
    # 1. free the repo's build outputs                                #
    #----------------------------------------------------------------#
    Step "Stopping related processes running from the repo"
    $hidden = Stop-RelatedUnder @($RepoRoot)
    $locked = Find-LockedRepoExes
    if($locked.Count -gt 0) {
        Info "still in use: $($locked -join ', ')"
        if($hidden -gt 0 -or -not (Test-IsElevated)) {
            $null = Invoke-Elevated "Sweep"
        }
        Start-Sleep -Milliseconds 500
        $locked = Find-LockedRepoExes
        if($locked.Count -gt 0) {
            throw "repo executables are still in use: $($locked -join ', ') -- close the program holding them and re-run"
        }
    }

    #----------------------------------------------------------------#
    # 2. vendored backend (pinned qmake recipe)                       #
    #----------------------------------------------------------------#
    $vendorDir  = Join-Path $RepoRoot "vendor\OpenRGB"
    $backendExe = Join-Path $vendorDir "OpenRGB Windows 64-bit\OpenRGB.exe"
    if(-not $SkipBackend) {
        $recipe = Join-Path $vendorDir "scripts\build-windows.bat"
        if(-not (Test-Path $recipe)) { throw "backend recipe not found: $recipe" }
        Step "Building vendored backend (qmake + jom -- long on a cold tree)"
        Push-Location $vendorDir
        try {
            & cmd.exe /c "`"$recipe`" 6.8.3 2022 64"
            if($LASTEXITCODE -ne 0) {
                throw "backend build recipe failed (exit $LASTEXITCODE)"
            }
        } finally { Pop-Location }
        # Stamp the fresh build with its source hash: build-and-deploy.ps1
        # (step 3) refuses a backend whose stamp does not match.
        if(Test-Path $backendExe) {
            Write-BackendStamp $RepoRoot (Split-Path -Parent $backendExe)
        }
    }
    if(-not (Test-Path $backendExe)) {
        throw "pinned backend missing: $backendExe (re-run without -SkipBackend)"
    }
    Info "backend: $backendExe"

    #----------------------------------------------------------------#
    # 3. app build + stage (as the invoking user)                     #
    #----------------------------------------------------------------#
    $deployArgs = @("-Configuration",$Configuration,"-BuildDir",$BuildDir)
    if($Reconfigure) { $deployArgs += "-Reconfigure" }
    if($SkipBuild)   { $deployArgs += "-SkipBuild" }
    if($Sign)        { $deployArgs += "-Sign" }
    Step "Build + stage (packaging\windows\build-and-deploy.ps1)"
    Invoke-PsScript (Join-Path $PkgDir "build-and-deploy.ps1") $deployArgs

    #----------------------------------------------------------------#
    # 4. elevated: stop the install, deploy, protect it               #
    #----------------------------------------------------------------#
    if(-not (Test-Path $InstallDir)) {
        # Created as the invoking user when the parent allows it; an
        # admin-only parent (%ProgramFiles%) gets it from the elevated
        # Deploy step instead. Either way -UserSid names whose state\ +
        # profiles\local\ stay writable.
        try {
            New-Item -ItemType Directory -Force $InstallDir -ErrorAction Stop | Out-Null
            Info "created install dir (first install): $InstallDir"
        } catch [UnauthorizedAccessException] {
            Info "install dir will be created by the elevated deploy step: $InstallDir"
        }
    }
    $null = Invoke-Elevated "Deploy"

    #----------------------------------------------------------------#
    # 5. desktop shortcut (as the invoking user)                      #
    #----------------------------------------------------------------#
    $uiExe   = Join-Path $InstallDir "openrgb2-ui.exe"
    $desktop = [Environment]::GetFolderPath("Desktop")
    $linked  = $false
    try {
        $wshell = New-Object -ComObject WScript.Shell
        foreach($lnk in @(Get-ChildItem $desktop -Filter *.lnk `
                            -ErrorAction SilentlyContinue)) {
            if($wshell.CreateShortcut($lnk.FullName).TargetPath -eq $uiExe) {
                $linked = $true
                Info "desktop shortcut ok: $($lnk.Name)"
                break
            }
        }
    } catch { }
    if(-not $linked) {
        Step "Creating desktop shortcut"
        Invoke-PsScript (Join-Path $PkgDir "create-desktop-shortcut.ps1") `
            @("-InstallDir",$InstallDir)
    }

    Step "Done"
    Info "installed: $InstallDir"
    Info "launch:    desktop icon, or $uiExe --managed"
    return 0
}

$exitCode = 1
try {
    if($ElevatedPhase) {
        if(-not (Test-IsElevated)) { throw "-ElevatedPhase must run elevated" }
        $exitCode = if($ElevatedPhase -eq "Sweep") { Invoke-SweepPhase }
                    else { Invoke-DeployPhase }
    } else {
        $exitCode = Invoke-Main
    }
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
# The UAC child has its own console window: hold it open only when it
# failed, so the error stays readable.
if($ElevatedPhase -and $exitCode -ne 0 -and -not $NoPause) {
    Write-Host ""
    Read-Host "Press Enter to close" | Out-Null
}
exit $exitCode
