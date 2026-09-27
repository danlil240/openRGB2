<#
.SYNOPSIS
    Update an installed OpenRGB2 release to a new package (Task 6.2).

.DESCRIPTION
    Installs a staged package (produced by stage-release.ps1) over an
    existing install while preserving ALL user state:

      Preserved (never overwritten, never deleted):
        profiles\local\   durable engine profile + resolved identities
        state\            backend private config + run file + journal

      Update flow:
        1. verify the NEW package: manifest.json parses and every
           listed file's SHA-256 matches (a corrupt/incomplete package
           never touches the install) -- skip with -SkipVerify.
        2. refuse if the install is RUNNING (verified by image path --
           stop it first with scripts\stop-openrgb2.ps1). Updating over
           running binaries is refused, not attempted.
        3. snapshot the ENTIRE current install to
           <InstallDir>.previous (one-level rollback; an existing
           .previous is replaced -- that is the documented depth).
        4. manifest-diff delete: files listed in the OLD install
           manifest but absent from the NEW manifest are removed
           (stale binaries), user files are never touched.
        5. copy the new package files over the install, skipping the
           preserved paths defensively.
        6. journal the update in state\updates.json.

    Rollback: scripts\rollback-openrgb2.ps1 restores the previous
    binaries from <InstallDir>.previous (manifest-verified).

    Exit codes: 0 updated, 1 failure (nothing or partially applied --
    the .previous snapshot is always taken BEFORE any change).

.EXAMPLE
    powershell -File scripts\update-openrgb2.ps1 -PackageDir build\package\OpenRGB2-0.2.0-win64 -InstallDir C:\OpenRGB2
#>
[CmdletBinding()]
param(
    # The new staged package dir (contains manifest.json). Required at
    # run time; validated in the body so the file stays dot-sourceable.
    [string]$PackageDir,

    # Install root. Default: parent of this script's dir when it looks
    # like an install (contains openrgb2-engine.exe); else required.
    [string]$InstallDir,

    # Skip SHA-256 verification of the incoming package (documented
    # escape hatch -- verification is the default, never silent).
    [switch]$SkipVerify
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

# User-state roots preserved across update AND rollback. Relative
# paths, forward or back slashes.
$script:PreservedPrefixes = @("profiles/local", "state")

function Test-PreservedPath([string]$rel) {
    $r = $rel -replace '\\','/'
    foreach($p in $script:PreservedPrefixes) {
        if($r -eq $p -or $r.StartsWith("$p/")) { return $true }
    }
    return $false
}

function Read-Manifest([string]$dir) {
    $m = Join-Path $dir "manifest.json"
    if(-not (Test-Path $m)) { return $null }
    try { return (Get-Content $m -Raw | ConvertFrom-Json) }
    catch { return $null }
}

# SHA-256-verify every file a manifest lists. Returns mismatch list.
function Test-ManifestFiles([string]$dir, $manifest) {
    $bad = @()
    foreach($f in $manifest.files) {
        $p = Join-Path $dir ($f.path -replace '/','\')
        if(-not (Test-Path $p)) { $bad += "missing: $($f.path)"; continue }
        $h = (Get-FileHash -Algorithm SHA256 $p).Hash.ToLower()
        if($h -ne $f.sha256) { $bad += "hash mismatch: $($f.path)" }
    }
    return ,$bad
}

# True when the install's engine or pinned backend is running --
# verified by IMAGE PATH under the install dir (never just a name).
function Test-InstallRunning([string]$InstallDir) {
    $engine  = Join-Path $InstallDir "openrgb2-engine.exe"
    $backend = Join-Path $InstallDir "backend\OpenRGB.exe"
    foreach($spec in @(@("openrgb2-engine",$engine), @("OpenRGB",$backend))) {
        $hit = Get-Process -Name $spec[0] -ErrorAction SilentlyContinue |
            Where-Object { try {
                $_.Path -and $_.Path.Equals($spec[1],
                    [StringComparison]::OrdinalIgnoreCase)
            } catch { $false } } | Select-Object -First 1
        if($hit) { return [pscustomobject]@{ name=$spec[0]; pid=$hit.Id } }
    }
    return $null
}

function Invoke-PackageUpdate {
    param(
        [string]$PackageDir,
        [string]$InstallDir,
        [bool]$SkipVerify
    )

    if(-not $InstallDir) {
        $candidate = Split-Path -Parent $PSScriptRoot
        if(Test-Path (Join-Path $candidate "openrgb2-engine.exe")) {
            $InstallDir = $candidate
        } else {
            Write-Host "ERROR: -InstallDir required (script dir's parent is not an install)" -ForegroundColor Red
            return 1
        }
    }
    $InstallDir = (Resolve-Path $InstallDir).Path
    if(-not $PackageDir) {
        Write-Host "ERROR: -PackageDir (the new staged package) is required" -ForegroundColor Red
        return 1
    }
    $PackageDir = (Resolve-Path $PackageDir).Path

    Write-Host "==> OpenRGB2 update: $PackageDir -> $InstallDir" -ForegroundColor Cyan

    #---- 1. verify the incoming package -------------------------------
    $newManifest = Read-Manifest $PackageDir
    if(-not $newManifest) {
        Write-Host "ERROR: $PackageDir has no readable manifest.json -- not a staged package" -ForegroundColor Red
        return 1
    }
    foreach($req in @("openrgb2-ui.exe","openrgb2-engine.exe",
                     "openrgb2-ctl.exe","backend/OpenRGB.exe")) {
        if(-not (Test-Path (Join-Path $PackageDir ($req -replace '/','\')))) {
            Write-Host "ERROR: package missing required file $req" -ForegroundColor Red
            return 1
        }
    }
    if(-not $SkipVerify) {
        Write-Host "  verifying package against manifest.json ($($newManifest.files.Count) files)..."
        $bad = Test-ManifestFiles $PackageDir $newManifest
        if($bad.Count -gt 0) {
            Write-Host "ERROR: package failed manifest verification -- refusing to install a corrupt package:" -ForegroundColor Red
            $bad | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
            return 1
        }
        Write-Host "  package verified (version $($newManifest.version))"
    }

    #---- 2. refuse when running ---------------------------------------
    $running = Test-InstallRunning $InstallDir
    if($running) {
        Write-Host "ERROR: install is running ($($running.name) pid $($running.pid)) -- stop it first: scripts\stop-openrgb2.ps1" -ForegroundColor Red
        return 1
    }

    $oldManifest = Read-Manifest $InstallDir
    $backupDir   = "$InstallDir.previous"

    #---- 3. snapshot the current install (before ANY change) ----------
    Write-Host "==> snapshotting current install -> $backupDir" -ForegroundColor Cyan
    if(Test-Path $backupDir) { Remove-Item -Recurse -Force $backupDir }
    Copy-Item -Recurse -Force $InstallDir $backupDir

    #---- 4. manifest-diff delete: drop files the update removes -------
    $newPaths = @{}
    foreach($f in $newManifest.files) { $newPaths[$f.path] = $true }
    if($oldManifest) {
        foreach($f in $oldManifest.files) {
            if(-not $newPaths.ContainsKey($f.path) -and
               -not (Test-PreservedPath $f.path)) {
                $p = Join-Path $InstallDir ($f.path -replace '/','\')
                if(Test-Path $p) {
                    Remove-Item -Force $p
                    Write-Host "  removed stale file: $($f.path)"
                }
            }
        }
    }

    #---- 5. copy the new package in, preserving user state ------------
    Write-Host "==> applying package files" -ForegroundColor Cyan
    $copied = 0
    foreach($f in $newManifest.files) {
        if(Test-PreservedPath $f.path) { continue }   # paranoid: a
        # package never ships preserved paths -- assert-and-skip anyway
        $src = Join-Path $PackageDir ($f.path -replace '/','\')
        $dst = Join-Path $InstallDir ($f.path -replace '/','\')
        if(-not (Test-Path $src)) { continue }
        $dstDir = Split-Path -Parent $dst
        if(-not (Test-Path $dstDir)) {
            New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        }
        Copy-Item -Force $src $dst
        ++$copied
    }
    # manifest.json itself is not in its own files list — copy it
    # explicitly so the install's manifest always describes the
    # CURRENT binaries (rollback's diff needs the true new set).
    Copy-Item -Force (Join-Path $PackageDir "manifest.json") $InstallDir
    Write-Host "  $copied files applied (+ manifest.json)"

    #---- 6. journal -----------------------------------------------------
    $stateDir = Join-Path $InstallDir "state"
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    $journal = Join-Path $stateDir "updates.json"
    $entries = @()
    if(Test-Path $journal) {
        try {
            # PS5 emits a JSON array as ONE Object[] into the pipe —
            # @() over the RESULT enumerates it; a bare object yields 1.
            $read = Get-Content $journal -Raw | ConvertFrom-Json
            $entries = @($read)
        } catch { $entries = @() }
    }
    $entries += [pscustomobject][ordered]@{
        utc          = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        action       = "update"
        from_version = if($oldManifest) { $oldManifest.version } else { "unknown" }
        to_version   = $newManifest.version
        backup       = $backupDir
    }
    ($entries | ConvertTo-Json -Depth 4) | Set-Content -Encoding UTF8 $journal

    Write-Host "UPDATE OK: $($newManifest.version) installed; rollback: scripts\rollback-openrgb2.ps1" -ForegroundColor Green
    return 0
}

if($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-PackageUpdate -PackageDir $PackageDir `
        -InstallDir $InstallDir -SkipVerify $SkipVerify)
}
