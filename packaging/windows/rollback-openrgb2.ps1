<#
.SYNOPSIS
    Roll back an OpenRGB2 install to the previous binaries (Task 6.2).

.DESCRIPTION
    Restores the install snapshot taken by update-openrgb2.ps1
    (<InstallDir>.previous) -- a MANIFEST-VERIFIED restore:

      1. the backup must exist and carry a readable manifest.json whose
         paths are all safe relative paths (no rooted/drive/UNC form,
         no '..' segment);
      2. every manifest-listed file in the backup is SHA-256-verified
         BEFORE anything is restored -- a corrupt/tampered backup is
         refused, not half-restored (-SkipVerify is the documented
         escape hatch);
      3. refuses while the install may be running: image path is this
         install's, image path UNREADABLE from this shell (elevated
         stack -- unverifiable, re-run elevated), or an install
         executable that cannot be opened for exclusive write;
      4. files in the CURRENT manifest -- and, after an interrupted
         update, every file that update was installing (its
         .update-in-progress.json marker) -- but absent from the
         backup manifest are removed (binaries the update added);
      5. backup files are copied back -- EXCEPT preserved user state:
         profiles\local\ and state\ are NEVER rolled back. Rollback
         restores binaries only; user state is sacred and stays at
         whatever the newest run wrote;
      6. the backup's manifest.json is restored last, so the install's
         manifest (and the version it reports) describes the restored
         binaries; an interrupted-update marker is cleared.

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

# Helpers are duplicated from update-openrgb2.ps1 on purpose: each
# lifecycle script is standalone under scripts\ (dot-sourcing the
# update script would rebind this script's own parameters).
$script:PreservedPrefixes = @("profiles/local", "state")

function Test-PreservedPath([string]$rel) {
    $r = ($rel -replace '\\','/').TrimStart('/')
    foreach($p in $script:PreservedPrefixes) {
        if($r.Equals($p, [StringComparison]::OrdinalIgnoreCase) -or
           $r.StartsWith("$p/", [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Test-SafeRelPath([string]$rel) {
    if([string]::IsNullOrWhiteSpace($rel)) { return $false }
    if($rel -match '[:*?"<>|]' -or $rel.IndexOf([char]0) -ge 0) { return $false }
    $r = $rel -replace '\\','/'
    if($r.StartsWith('/')) { return $false }
    foreach($seg in $r.Split('/')) {
        if($seg -eq '' -or $seg -eq '.' -or $seg -eq '..') { return $false }
        if($seg.TrimEnd(' ','.') -ne $seg) { return $false }
    }
    return $true
}

function Get-UnsafeManifestPaths($manifest) {
    $bad = @()
    foreach($f in @($manifest.files)) {
        if(-not (Test-SafeRelPath ([string]$f.path))) { $bad += [string]$f.path }
    }
    return ,$bad
}

function Read-Manifest([string]$dir) {
    $m = Join-Path $dir "manifest.json"
    if(-not (Test-Path -LiteralPath $m)) { return $null }
    try { return (Get-Content -LiteralPath $m -Raw | ConvertFrom-Json) }
    catch { return $null }
}

function Read-UpdateMarker([string]$InstallDir) {
    $m = Join-Path $InstallDir ".update-in-progress.json"
    if(-not (Test-Path -LiteralPath $m)) { return $null }
    try { return (Get-Content -LiteralPath $m -Raw | ConvertFrom-Json) }
    catch { return [pscustomobject]@{ from_version = ""; to_files = @() } }
}

function Test-ManifestFiles([string]$dir, $manifest) {
    $bad = @()
    foreach($f in $manifest.files) {
        $p = Join-Path $dir ($f.path -replace '/','\')
        if(-not (Test-Path -LiteralPath $p)) { $bad += "missing: $($f.path)"; continue }
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower()
        if($h -ne $f.sha256) { $bad += "hash mismatch: $($f.path)" }
    }
    return ,$bad
}

function Find-LockedExecutables([string]$InstallDir) {
    $locked = @(); $denied = @()
    foreach($rel in @("openrgb2-ui.exe","openrgb2-engine.exe",
                      "openrgb2-ctl.exe","backend\OpenRGB.exe")) {
        $p = Join-Path $InstallDir $rel
        if(-not (Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        try {
            $fs = [IO.File]::Open($p, [IO.FileMode]::Open,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $fs.Dispose()
        } catch [UnauthorizedAccessException] {
            $denied += $rel
        } catch {
            $locked += $rel
        }
    }
    return [pscustomobject]@{ Locked = $locked; Denied = $denied }
}

# Same rules as update-openrgb2.ps1's Test-InstallRunning.
function Test-InstallRunning([string]$InstallDir, [object[]]$ProcessList) {
    $useList = $PSBoundParameters.ContainsKey('ProcessList')
    $unverifiable = @()
    foreach($spec in @(@("openrgb2-engine","openrgb2-engine.exe"),
                       @("openrgb2-ui","openrgb2-ui.exe"),
                       @("OpenRGB","backend\OpenRGB.exe"))) {
        $expected = Join-Path $InstallDir $spec[1]
        $procs = if($useList) {
                     @($ProcessList | Where-Object { $_.ProcessName -eq $spec[0] })
                 } else {
                     @(Get-Process -Name $spec[0] -ErrorAction SilentlyContinue)
                 }
        foreach($p in $procs) {
            $img = $null
            try { $img = $p.Path } catch { $img = $null }
            if(-not $img) { $unverifiable += "$($spec[0]) pid $($p.Id)"; continue }
            if($img.Equals($expected, [StringComparison]::OrdinalIgnoreCase)) {
                return [pscustomobject]@{ kind="running"; name=$spec[0]; pid=$p.Id; detail="" }
            }
        }
    }
    if($unverifiable.Count -gt 0) {
        return [pscustomobject]@{ kind="unverifiable"; name=""; pid=0
                                  detail=($unverifiable -join ', ') }
    }
    $probe = Find-LockedExecutables $InstallDir
    if($probe.Locked.Count -gt 0) {
        return [pscustomobject]@{ kind="locked"; name=""; pid=0
                                  detail=($probe.Locked -join ', ') }
    }
    if($probe.Denied.Count -gt 0) {
        return [pscustomobject]@{ kind="denied"; name=""; pid=0
                                  detail=($probe.Denied -join ', ') }
    }
    return $null
}

function Write-RunningRefusal($running, [string]$verb) {
    $stopHint = "stop it first: scripts\stop-openrgb2.ps1 -Force " +
                "(also closes the UI), or use the tray menu 'Quit OpenRGB2'"
    switch($running.kind) {
        "running" {
            Write-Host "ERROR: install is running ($($running.name) pid $($running.pid)) -- $stopHint" -ForegroundColor Red }
        "unverifiable" {
            Write-Host ("ERROR: same-named process(es) are running whose image " +
                "path is UNREADABLE from this shell ($($running.detail)) -- an " +
                "elevated OpenRGB2 stack cannot be told apart from a foreign " +
                "one. Nothing was changed: stop it (scripts\stop-openrgb2.ps1 " +
                "-Elevated -Force) and re-run the $verb from an elevated " +
                "PowerShell.") -ForegroundColor Red }
        "locked" {
            Write-Host ("ERROR: install executables are in use " +
                "($($running.detail)) -- a process still has them loaded; " +
                "$stopHint") -ForegroundColor Red }
        "denied" {
            Write-Host ("ERROR: install executables are not writable from " +
                "this shell ($($running.detail)) -- the install is protected " +
                "(admin-only); re-run the $verb from an elevated " +
                "PowerShell.") -ForegroundColor Red }
    }
}

function Invoke-PackageRollback {
    param(
        [string]$InstallDir,
        [string]$BackupDir,
        [bool]$SkipVerify,
        # offline-test seam forwarded to Test-InstallRunning
        [object[]]$ProcessList
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

    if(-not (Test-Path -LiteralPath $BackupDir)) {
        Write-Host "ERROR: no rollback snapshot at $BackupDir -- update-openrgb2.ps1 creates it before applying a package" -ForegroundColor Red
        return 1
    }

    #---- manifest-verified restore ------------------------------------
    $bakManifest = Read-Manifest $BackupDir
    if(-not $bakManifest) {
        Write-Host "ERROR: backup has no readable manifest.json -- cannot verify the restore" -ForegroundColor Red
        return 1
    }
    $unsafe = Get-UnsafeManifestPaths $bakManifest
    if($unsafe.Count -gt 0) {
        Write-Host "ERROR: backup manifest lists unsafe paths (rooted / '..' / ADS) -- refusing: $($unsafe -join ', ')" -ForegroundColor Red
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

    $running = if($PSBoundParameters.ContainsKey('ProcessList')) {
                   Test-InstallRunning $InstallDir -ProcessList $ProcessList
               } else { Test-InstallRunning $InstallDir }
    if($running) {
        Write-RunningRefusal $running "rollback"
        return 1
    }

    #---- remove files the update added ---------------------------------
    # Candidates: the current manifest's files plus -- after an
    # interrupted update, whose manifest.json was never replaced --
    # every file that update was installing (its marker).
    $curManifest = Read-Manifest $InstallDir
    $candidates  = @()
    if($curManifest) {
        $unsafe = Get-UnsafeManifestPaths $curManifest
        if($unsafe.Count -gt 0) {
            Write-Host "ERROR: the INSTALLED manifest lists unsafe paths -- refusing: $($unsafe -join ', ')" -ForegroundColor Red
            return 1
        }
        $candidates += @($curManifest.files | ForEach-Object { [string]$_.path })
    }
    $marker = Read-UpdateMarker $InstallDir
    if($marker) {
        Write-Host "  an interrupted update was in progress -- rolling back over it"
        foreach($p in @($marker.to_files)) {
            if($p -and (Test-SafeRelPath ([string]$p))) { $candidates += [string]$p }
        }
    }
    $bakPaths = @{}
    foreach($f in $bakManifest.files) { $bakPaths[[string]$f.path] = $true }
    foreach($rel in @($candidates | Sort-Object -Unique)) {
        if(-not $bakPaths.ContainsKey($rel) -and -not (Test-PreservedPath $rel)) {
            $p = Join-Path $InstallDir ($rel -replace '/','\')
            if(Test-Path -LiteralPath $p) {
                Remove-Item -LiteralPath $p -Force
                Write-Host "  removed update-added file: $rel"
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
        if(-not (Test-Path -LiteralPath $src)) { continue }
        $dstDir = Split-Path -Parent $dst
        if(-not (Test-Path -LiteralPath $dstDir)) {
            New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        }
        Copy-Item -Force -LiteralPath $src -Destination $dst
        ++$restored
    }
    # manifest.json is not in its own files list -- restore it LAST so
    # the install's manifest (version, file set for the next update's
    # diff) describes the binaries now in place.
    Copy-Item -Force -LiteralPath (Join-Path $BackupDir "manifest.json") -Destination $InstallDir
    $markerPath = Join-Path $InstallDir ".update-in-progress.json"
    if(Test-Path -LiteralPath $markerPath) { Remove-Item -LiteralPath $markerPath -Force }
    Write-Host "  $restored files restored (+ manifest.json); profiles\local\ + state\ untouched"

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
    $fromVersion = if($curManifest) { $curManifest.version } else { "unknown" }
    if($marker -and $marker.PSObject.Properties['to_version'] -and $marker.to_version) {
        $fromVersion = "$($marker.to_version) (interrupted)"
    }
    $entries += [pscustomobject][ordered]@{
        utc          = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        action       = "rollback"
        from_version = $fromVersion
        to_version   = $bakManifest.version
        backup       = $BackupDir
    }
    # -InputObject keeps the journal a JSON ARRAY even with a single
    # entry (piping would unroll it to a bare object -- shape drift).
    (ConvertTo-Json -InputObject $entries -Depth 4) |
        Set-Content -Encoding UTF8 $journal

    Write-Host "ROLLBACK OK: restored $($bakManifest.version) binaries; user state preserved" -ForegroundColor Green
    return 0
}

if($MyInvocation.InvocationName -ne '.') {
    # An elevated run must not auto-load modules from the user's own
    # (user-writable) Documents module path.
    try {
        $isAdmin = (New-Object Security.Principal.WindowsPrincipal(
            [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { $isAdmin = $false }
    if($isAdmin) {
        $env:PSModulePath = (Join-Path $env:ProgramFiles "WindowsPowerShell\Modules") +
            ";" + (Join-Path $PSHOME "Modules")
    }
    exit (Invoke-PackageRollback -InstallDir $InstallDir `
        -BackupDir $BackupDir -SkipVerify $SkipVerify)
}
