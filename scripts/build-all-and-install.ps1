<#
.SYNOPSIS
    Elevated build-everything + install-in-place for OpenRGB2.

.DESCRIPTION
    One command from repo checkout to a working desktop icon:

      1. Elevates via UAC -- the managed stack runs elevated, so only
         an elevated shell can verify and stop its processes.
      2. Stops every related process: the installed managed stack via
         <InstallDir>\scripts\stop-openrgb2.ps1 -Force (engine IPC QUIT
         -> backend shutdown event -> verified Stop-Process fallback),
         then a sweep that force-stops any openrgb2/OpenRGB binary still
         running from this repo (build\ outputs, staged packages, a
         vendor\OpenRGB backend started in place) or from the install
         dir. Foreign installs are listed, never touched.
      3. Builds the pinned vendored backend via
         vendor\OpenRGB\scripts\build-windows.bat 6.8.3 2022 64
         (qmake + jom, incremental; -SkipBackend to skip).
      4. Runs packaging\windows\build-and-deploy.ps1: CMake build of
         engine + UI + tools -> stage-release.ps1 (windeployqt +
         manifest) -> update-openrgb2.ps1 into -InstallDir
         (manifest-verified, preserves profiles\local\ + state\,
         one-level .previous rollback).
      5. Ensures a desktop shortcut targets <InstallDir>\openrgb2-ui.exe
         (creates OpenRGB2.lnk via create-desktop-shortcut.ps1 when
         none does).

    Default InstallDir = %LOCALAPPDATA%\Programs\OpenRGB2 -- the target
    of the OpenRGB2 desktop icon.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-all-and-install.ps1

.EXAMPLE
    scripts\build-all-and-install.cmd          (double-clickable)
#>
[CmdletBinding()]
param(
    # Install root the desktop icon launches. Default:
    # %LOCALAPPDATA%\Programs\OpenRGB2.
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Programs\OpenRGB2"),

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

    # Internal: marks the UAC-relaunched instance.
    [switch]$ElevatedChild,

    # Don't hold the elevated window open at the end.
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$RepoRoot  = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$PkgDir    = Join-Path $RepoRoot "packaging\windows"
# Script-level bound params -- captured here because $PSBoundParameters
# inside Invoke-Main would be the function's own (empty) table.
$BoundArgs = $PSBoundParameters

function Info([string]$msg) { Write-Host "  $msg" }
function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

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

function Invoke-Main {
    #----------------------------------------------------------------#
    # 0. elevation                                                   #
    #----------------------------------------------------------------#
    $principal = New-Object Security.Principal.WindowsPrincipal(
                     [Security.Principal.WindowsIdentity]::GetCurrent())
    if(-not $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Step "elevation required -- re-launching under UAC"
        $argList = @("-NoProfile","-ExecutionPolicy","Bypass","-File",
                     "`"$PSCommandPath`"","-ElevatedChild")
        foreach($k in $BoundArgs.Keys) {
            if($k -eq "ElevatedChild") { continue }
            $v = $BoundArgs[$k]
            if($v -is [System.Management.Automation.SwitchParameter]) {
                if($v.IsPresent) { $argList += "-$k" }
            } else {
                $argList += @("-$k","`"$v`"")
            }
        }
        try {
            $proc = Start-Process -FilePath powershell.exe -Verb RunAs `
                        -Wait -PassThru -ArgumentList ($argList -join " ")
        } catch {
            throw "elevation declined or unavailable: $($_.Exception.Message)"
        }
        return $proc.ExitCode
    }

    Step "OpenRGB2 build-all + install"
    Info "repo:    $RepoRoot"
    Info "install: $InstallDir"
    Info "config:  $Configuration"

    #----------------------------------------------------------------#
    # 1. stop the installed managed stack (graceful -> forced)        #
    #----------------------------------------------------------------#
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

    #----------------------------------------------------------------#
    # 2. sweep: force-stop our own processes left anywhere            #
    #----------------------------------------------------------------#
    # Covers dev-run build\ outputs, staged packages, and a vendored
    # backend started in place -- anything whose exe would lock files
    # the build/stage/update steps write. Only image paths under the
    # repo or the install dir are touched; foreign installs are listed.
    Step "Killing leftover related processes"
    $names = @("openrgb2-ui","openrgb2-engine","openrgb2-ctl",
             "OpenRGB","lianli_bench","skew_bench")
    $roots = @($RepoRoot)
    if((Resolve-Path $InstallDir -ErrorAction SilentlyContinue) -and
       -not (Test-UnderRoot $InstallDir $RepoRoot)) {
        $roots += (Resolve-Path $InstallDir).Path
    }
    $killed = 0
    foreach($p in @(Get-Process -Name $names -ErrorAction SilentlyContinue)) {
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
        }
    }
    if($killed -eq 0) { Info "nothing related running" }

    #----------------------------------------------------------------#
    # 3. vendored backend (pinned qmake recipe)                       #
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
    }
    if(-not (Test-Path $backendExe)) {
        throw "pinned backend missing: $backendExe (re-run without -SkipBackend)"
    }
    Info "backend: $backendExe"

    #----------------------------------------------------------------#
    # 4. app build + stage + deploy into the icon's install dir       #
    #----------------------------------------------------------------#
    if(-not (Test-Path $InstallDir)) {
        New-Item -ItemType Directory -Force $InstallDir | Out-Null
        Info "created install dir (first install): $InstallDir"
    }
    $deployArgs = @("-Configuration",$Configuration,
                    "-InstallDir",$InstallDir,"-ForceStop")
    if($BuildDir)    { $deployArgs += @("-BuildDir",$BuildDir) }
    if($Reconfigure) { $deployArgs += "-Reconfigure" }
    if($SkipBuild)   { $deployArgs += "-SkipBuild" }
    if($Sign)        { $deployArgs += "-Sign" }
    Step "Build + stage + deploy (packaging\windows\build-and-deploy.ps1)"
    Invoke-PsScript (Join-Path $PkgDir "build-and-deploy.ps1") $deployArgs

    #----------------------------------------------------------------#
    # 5. desktop shortcut                                             #
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
try { $exitCode = Invoke-Main }
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
if($ElevatedChild -and -not $NoPause) {
    Write-Host ""
    Read-Host "Press Enter to close" | Out-Null
}
exit $exitCode
