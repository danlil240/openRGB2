<#
.SYNOPSIS
    One-command build + package (+ optional deploy) for OpenRGB2.

.DESCRIPTION
    Chains the documented release flow into a single step:

      1. CMake configure (skipped while the cache exists; -Reconfigure
         forces) + --build of the VS 2022 x64 tree for -Configuration.
      2. Pinned backend check: the vendored OpenRGB.exe must exist
         under vendor\OpenRGB ("OpenRGB Windows 64-bit" or release\)
         or in OPENRGB2_BACKEND_DIR. -BuildBackend runs the in-tree
         qmake recipe (long: full backend build) when it is missing.
         A vendored backend must also be current: the stamp the
         recipe step writes next to OpenRGB.exe
         (openrgb2-backend.stamp, see backend-stamp.ps1) must match
         a hash of vendor/OpenRGB + src/core/playback. A stale or
         unstamped backend is rebuilt with -BuildBackend (incremental
         qmake), staged with a warning under -AllowStaleBackend, and
         otherwise fails with "backend is stale". An
         OPENRGB2_BACKEND_DIR backend is not checked.
      3. Stop (only with -InstallDir): stops the running install via
         its own scripts\stop-openrgb2.ps1 (UI close -> graceful
         engine QUIT -> backend shutdown event; -ForceStop passes
         -Force) BEFORE staging, so nothing of the install can hold
         files the staging or update steps touch.
      4. packaging\windows\stage-release.ps1 assembles + verifies
         build\package\OpenRGB2-<version>-win64. -Sign passes through
         to Azure Trusted Signing; the account fields resolve from
         OPENRGB2_TS_* env vars as documented in stage-release.ps1.
      5. Deploy (only with -InstallDir): update-openrgb2.ps1 --
         manifest-verified, preserves profiles\local\ + state\,
         one-level .previous rollback; run elevated it also makes the
         install admin-only (see update-openrgb2.ps1).

    Without -InstallDir the staged folder IS the product -- run
    <OutDir>\OpenRGB2.cmd; no separate deploy step is needed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File packaging\windows\build-and-deploy.ps1

.EXAMPLE
    # Build, sign, and update an existing install
    powershell -File packaging\windows\build-and-deploy.ps1 -Sign -InstallDir D:\Apps\OpenRGB2
#>
[CmdletBinding()]
param(
    # CMake build tree. Default: <repo>\build.
    [string]$BuildDir,

    # Build configuration of app targets (and the staged inputs).
    [string]$Configuration = "Release",

    # cmake.exe override. Default: cmake on PATH, else the VS 2022
    # BuildTools-bundled binary (cmake is not on PATH here).
    [string]$CMakeExe,

    # Re-run CMake configure even when the cache already exists.
    [switch]$Reconfigure,

    # Skip the CMake build entirely -- stage from existing outputs.
    [switch]$SkipBuild,

    # Run vendor\OpenRGB\scripts\build-windows.bat when the pinned
    # backend binary is missing (long -- full qmake backend build) or
    # stale against its sources (incremental).
    [switch]$BuildBackend,

    # Stage a stale or unstamped vendored backend anyway, with a
    # warning, instead of failing.
    [switch]$AllowStaleBackend,

    # Authenticode-sign staged exes via Azure Trusted Signing
    # (passthrough to stage-release.ps1).
    [switch]$Sign,

    # Deploy target: an existing install dir to update after staging.
    [string]$InstallDir,

    # Pass -Force to the install's stop script -- the documented last
    # resort for verified-own processes after the graceful bound.
    [switch]$ForceStop
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

#--------------------------------------------------------------------#
# Resolve roots + helpers                                             #
#--------------------------------------------------------------------#
$PkgDir   = Split-Path -Parent $MyInvocation.MyCommand.Path          # packaging/windows
$RepoRoot = (Resolve-Path (Join-Path $PkgDir "..\..")).Path

if(-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "build" }

function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Info([string]$msg) { Write-Host "  $msg" }
function Warn([string]$msg) { Write-Host "  WARNING: $msg" -ForegroundColor Yellow }
function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# Get-BackendSourceHash / Write-BackendStamp / Test-BackendStamp
. (Join-Path $PkgDir "backend-stamp.ps1")

function Invoke-PsScript([string]$script, [string[]]$scriptArgs) {
    # The lifecycle scripts end in `exit <code>` -- invoke each in a
    # powershell -File subprocess so that exit lands in $LASTEXITCODE
    # instead of terminating this wrapper's own session mid-run.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script @scriptArgs
    if($LASTEXITCODE -ne 0) { Fail "$script failed (exit $LASTEXITCODE)" }
}

#--------------------------------------------------------------------#
# 1. configure + build                                                #
#--------------------------------------------------------------------#
if(-not $SkipBuild) {
    if(-not $CMakeExe) {
        $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
        if($onPath) { $CMakeExe = $onPath.Source }
        else {
            $CMakeExe = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        }
    }
    if(-not (Test-Path $CMakeExe)) {
        Fail "cmake not found (pass -CMakeExe -- neither PATH nor the VS-bundled copy resolved): $CMakeExe"
    }

    $qtDir = if($env:OPENRGB2_QT_DIR) { $env:OPENRGB2_QT_DIR }
             else                    { "C:\Qt\6.8.3\msvc2022_64" }

    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if($Reconfigure -or -not (Test-Path $cache)) {
        Step "Configuring $BuildDir (VS 17 2022 x64, Qt $qtDir)"
        & $CMakeExe -S $RepoRoot -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
            "-DCMAKE_PREFIX_PATH=$qtDir"
        if($LASTEXITCODE -ne 0) { Fail "cmake configure failed" }
    }
    # A cache configured without Qt builds no UI at all -- staging would
    # only fail much later on the missing openrgb2-ui.exe.
    if(Select-String -LiteralPath $cache -Pattern '^Qt6_DIR:[A-Z]+=.*-NOTFOUND' -Quiet) {
        Fail ("$cache has Qt6_DIR-NOTFOUND -- the UI cannot be built. Set " +
              "OPENRGB2_QT_DIR (default C:\Qt\6.8.3\msvc2022_64) and re-run " +
              "with -Reconfigure")
    }

    Step "Building $Configuration"
    & $CMakeExe --build $BuildDir --config $Configuration
    if($LASTEXITCODE -ne 0) { Fail "cmake build failed" }
} else {
    Info "build skipped (-SkipBuild)"
}

if(-not (Test-Path $BuildDir)) { Fail "build tree not found: $BuildDir" }
$BuildDir = (Resolve-Path $BuildDir).Path

#--------------------------------------------------------------------#
# 2. pinned vendored backend                                          #
#--------------------------------------------------------------------#
$backendCandidates = @()
if($env:OPENRGB2_BACKEND_DIR) { $backendCandidates += $env:OPENRGB2_BACKEND_DIR }
$backendCandidates += Join-Path $RepoRoot "vendor\OpenRGB\OpenRGB Windows 64-bit"
$backendCandidates += Join-Path $RepoRoot "vendor\OpenRGB\release"

function Find-Backend {
    foreach($d in $backendCandidates) {
        if(Test-Path (Join-Path $d "OpenRGB.exe")) { return $d }
    }
    return $null
}

# Run the in-tree qmake recipe (incremental on a warm tree), then
# stamp the result with the hash of the sources it was built from.
function Invoke-BackendRecipe {
    Step "Building pinned backend (vendor\OpenRGB\scripts\build-windows.bat)"
    $recipe = Join-Path $RepoRoot "vendor\OpenRGB\scripts\build-windows.bat"
    if(-not (Test-Path $recipe)) { Fail "backend recipe not found: $recipe" }
    # The qmake recipe resolves OpenRGB.pro relative to the working
    # directory -- it must run from vendor\OpenRGB.
    # Out-Host: the recipe's output must not become this function's
    # return value.
    Push-Location (Join-Path $RepoRoot "vendor\OpenRGB")
    try {
        & cmd.exe /c "`"$recipe`" 6.8.3 2022 64" | Out-Host
        $recipeExit = $LASTEXITCODE
    } finally { Pop-Location }
    if($recipeExit -ne 0) { Fail "backend build recipe failed (exit $recipeExit)" }
    $built = Find-Backend
    if($built) { Write-BackendStamp $RepoRoot $built }
    return $built
}

$backendDir = Find-Backend

if(-not $backendDir -and $BuildBackend) {
    $backendDir = Invoke-BackendRecipe
}
if(-not $backendDir) {
    Fail ("pinned backend OpenRGB.exe not found under vendor\OpenRGB -- " +
          "re-run with -BuildBackend, or " +
          "vendor\OpenRGB\scripts\build-windows.bat 6.8.3 2022 64 " +
          "(or set OPENRGB2_BACKEND_DIR)")
}

# Staleness: the vendored backend must have been built from the
# current vendor/OpenRGB + src/core/playback sources.
if($env:OPENRGB2_BACKEND_DIR -and $backendDir -eq $env:OPENRGB2_BACKEND_DIR) {
    Info "backend from OPENRGB2_BACKEND_DIR: staleness not checked"
} else {
    $stamp = Test-BackendStamp $RepoRoot $backendDir
    if($stamp.State -eq "fresh") {
        Info $stamp.Message
    } elseif($stamp.State -eq "unknown") {
        Warn $stamp.Message
    } elseif($BuildBackend) {
        Info "$($stamp.Message) -- rebuilding (-BuildBackend)"
        $backendDir = Invoke-BackendRecipe
        if(-not $backendDir) { Fail "backend recipe ran but OpenRGB.exe is still missing" }
        $stamp = Test-BackendStamp $RepoRoot $backendDir
        if($stamp.State -ne "fresh" -and $stamp.State -ne "unknown") {
            Fail "$($stamp.Message) (even after the rebuild)"
        }
        Info $stamp.Message
    } elseif($AllowStaleBackend) {
        Warn "$($stamp.Message) -- staging it anyway (-AllowStaleBackend)"
    } else {
        Fail ("$($stamp.Message). Re-run with -BuildBackend (incremental qmake " +
              "rebuild), or -AllowStaleBackend to stage it anyway")
    }
}
Info "backend: $backendDir"

#--------------------------------------------------------------------#
# 3. stop the running install (before staging)                        #
#--------------------------------------------------------------------#
if($InstallDir) {
    if(-not (Test-Path $InstallDir)) {
        Fail "-InstallDir does not exist: $InstallDir"
    }
    $InstallDir = (Resolve-Path $InstallDir).Path

    # update-openrgb2.ps1 refuses to apply over a running install
    # (UI included), and its scripts\ stop is the managed path (UI
    # close -> engine IPC QUIT -> backend shutdown event; verified-own
    # only). Stopping BEFORE staging means a stack that runs from (or
    # holds files of) the package dir cannot fail the staging wipe.
    $stopScript = Join-Path $InstallDir "scripts\stop-openrgb2.ps1"
    if(Test-Path $stopScript) {
        Step "Stopping running install ($InstallDir)"
        $stopArgs = @("-InstallDir", $InstallDir)
        if($ForceStop) { $stopArgs += "-Force" }
        Invoke-PsScript $stopScript $stopArgs
    }
}

#--------------------------------------------------------------------#
# 4. stage + verify                                                   #
#--------------------------------------------------------------------#
# Same default OutDir as stage-release.ps1 (version from CMakeLists).
$cmakeTop = Get-Content (Join-Path $RepoRoot "CMakeLists.txt") -Raw
$version  = "0.0.0"
if($cmakeTop -match "project\s*\(\s*OpenRGB2\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)") {
    $version = $Matches[1]
}
$OutDir = Join-Path $BuildDir "package\OpenRGB2-$version-win64"

Step "Staging release package -> $OutDir"
$stageArgs = @("-BuildDir", $BuildDir,
             "-Configuration", $Configuration,
             "-OutDir", $OutDir)
if($Sign) { $stageArgs += "-Sign" }
Invoke-PsScript (Join-Path $PkgDir "stage-release.ps1") $stageArgs

#--------------------------------------------------------------------#
# 5. deploy (optional)                                                #
#--------------------------------------------------------------------#
if($InstallDir) {
    Step "Applying package -> $InstallDir"
    Invoke-PsScript (Join-Path $PkgDir "update-openrgb2.ps1") `
        @("-PackageDir", $OutDir, "-InstallDir", $InstallDir)
}

#--------------------------------------------------------------------#
# Summary                                                             #
#--------------------------------------------------------------------#
Step "Done"
Info "package: $OutDir"
Info "run:     $(Join-Path $OutDir 'OpenRGB2.cmd')"
if($InstallDir) {
    Info "install: $InstallDir (rollback: $(Join-Path $InstallDir 'scripts\rollback-openrgb2.ps1'))"
}
