<#
.SYNOPSIS
    Roll back an OpenRGB2 install to the previous binaries (Task 6.2).

.DESCRIPTION
    Restores the install snapshot taken by update-openrgb2.ps1
    (<InstallDir>.previous) -- a MANIFEST-VERIFIED restore:

      1. the backup must exist and carry a readable manifest.json;
      2. every manifest-listed file in the backup is SHA-256-verified
         BEFORE anything is restored -- a corrupt/tampered backup is
         refused, not half-restored (-SkipVerify is the documented
         escape hatch);
      3. refuses while the install is running (image-path verified);
      4. files in the CURRENT manifest but absent from the backup
         manifest are removed (binaries the update added);
      5. backup files are copied back -- EXCEPT preserved user state:
         profiles\local\ and state\ are NEVER rolled back. Rollback
         restores binaries only; user state is sacred and stays at
         whatever the newest run wrote.

    Exit codes: 0 restored, 1 failure (backup missing/corrupt,
    running, or copy failure).

.EXAMPLE
    powershell -File scripts\rollback-openrgb2.ps1 -InstallDir C:\OpenRGB2
#>
[CmdletBinding()]
param(
    # Install root. Default: parent of this script's dir when it looks
    # like an install.
    [string]$InstallDir,

    # Snapshot dir. Default: <InstallDir>.previous
    [string]$BackupDir,

    # Skip SHA-256 verification of the backup before restore.
    [switch]$SkipVerify
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

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

function Invoke-PackageRollback {
    param(
        [string]$InstallDir,
        [string]$BackupDir,
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
    if(-not $BackupDir) { $BackupDir = "$InstallDir.previous" }

    Write-Host "==> OpenRGB2 rollback: $BackupDir -> $InstallDir" -ForegroundColor Cyan

    if(-not (Test-Path $BackupDir)) {
        Write-Host "ERROR: no rollback snapshot at $BackupDir -- update-openrgb2.ps1 creates it before applying a package" -ForegroundColor Red
        return 1
    }

    #---- manifest-verified restore ------------------------------------
    $bakManifest = Read-Manifest $BackupDir
    if(-not $bakManifest) {
        Write-Host "ERROR: backup has no readable manifest.json -- cannot verify the restore" -ForegroundColor Red
        return 1
    }
    if(-not $SkipVerify) {
        Write-Host "  verifying backup against its manifest ($($bakManifest.files.Count) files)..."
        $bad = Test-ManifestFiles $BackupDir $bakManifest
        if($bad.Count -gt 0) {
            Write-Host "ERROR: backup failed manifest verification -- refusing to restore corrupt binaries:" -ForegroundColor Red
            $bad | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
            return 1
        }
        Write-Host "  backup verified (version $($bakManifest.version))"
    }

    $running = Test-InstallRunning $InstallDir
    if($running) {
        Write-Host "ERROR: install is running ($($running.name) pid $($running.pid)) -- stop it first: scripts\stop-openrgb2.ps1" -ForegroundColor Red
        return 1
    }

    #---- remove files the update added ---------------------------------
    $bakPaths = @{}
    foreach($f in $bakManifest.files) { $bakPaths[$f.path] = $true }
    $curManifest = Read-Manifest $InstallDir
    if($curManifest) {
        foreach($f in $curManifest.files) {
            if(-not $bakPaths.ContainsKey($f.path) -and
               -not (Test-PreservedPath $f.path)) {
                $p = Join-Path $InstallDir ($f.path -replace '/','\')
                if(Test-Path $p) {
                    Remove-Item -Force $p
                    Write-Host "  removed update-added file: $($f.path)"
                }
            }
        }
    }

    #---- restore backup files (never preserved user state) --------------
    Write-Host "==> restoring previous binaries" -ForegroundColor Cyan
    $restored = 0
    foreach($f in $bakManifest.files) {
        if(Test-PreservedPath $f.path) { continue }
        $src = Join-Path $BackupDir ($f.path -replace '/','\')
        $dst = Join-Path $InstallDir ($f.path -replace '/','\')
        if(-not (Test-Path $src)) { continue }
        $dstDir = Split-Path -Parent $dst
        if(-not (Test-Path $dstDir)) {
            New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        }
        Copy-Item -Force $src $dst
        ++$restored
    }
    Write-Host "  $restored files restored; profiles\local\ + state\ untouched"

    #---- journal ---------------------------------------------------------
    $stateDir = Join-Path $InstallDir "state"
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    $journal = Join-Path $stateDir "updates.json"
    $entries = @()
    if(Test-Path $journal) {
        try {
            $read = Get-Content $journal -Raw | ConvertFrom-Json
            $entries = @($read)   # PS5 emits JSON arrays un-enumerated
        } catch { $entries = @() }
    }
    $entries += [pscustomobject][ordered]@{
        utc          = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        action       = "rollback"
        from_version = if($curManifest) { $curManifest.version } else { "unknown" }
        to_version   = $bakManifest.version
        backup       = $BackupDir
    }
    ($entries | ConvertTo-Json -Depth 4) | Set-Content -Encoding UTF8 $journal

    Write-Host "ROLLBACK OK: restored $($bakManifest.version) binaries; user state preserved" -ForegroundColor Green
    return 0
}

if($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-PackageRollback -InstallDir $InstallDir `
        -BackupDir $BackupDir -SkipVerify $SkipVerify)
}
