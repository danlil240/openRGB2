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
        1. verify the NEW package: manifest.json parses, every listed
           path is a safe relative path (no rooted/drive/UNC form, no
           '..' segment, no ADS colon) and every listed file's SHA-256
           matches (a corrupt/incomplete package never touches the
           install) -- skip the hash passes with -SkipVerify.
        2. refuse if the install may be RUNNING: a same-named process
           whose image path is this install's (stop it first), a
           same-named process whose image path is UNREADABLE from this
           shell (an elevated stack seen from a non-elevated shell --
           unverifiable, re-run elevated), or an install executable
           that cannot be opened for exclusive write (still loaded by
           some process). Nothing is changed when refused.
        3. snapshot the ENTIRE current install to
           <InstallDir>.previous.tmp and only then swap it in as
           <InstallDir>.previous (one-level rollback; the old snapshot
           is replaced only once the new one is complete), then write
           the marker <InstallDir>\.update-in-progress.json. While the
           marker exists the snapshot is NEVER retaken: a re-run
           resumes and finishes the interrupted update, and
           rollback-openrgb2.ps1 restores the snapshot.
        4. manifest-diff delete: files listed in the OLD install
           manifest but absent from the NEW manifest are removed
           (stale binaries), user files are never touched.
        5. copy the new package files over the install, skipping the
           preserved paths defensively; re-verify the installed files
           against the new manifest; only then copy manifest.json.
        6. journal the update in state\updates.json; clear the marker.
        7. ELEVATED runs harden the install ACL: owner Administrators,
           inheritance broken, Administrators + SYSTEM full control,
           Users read/execute -- except state\ and profiles\local\,
           which stay writable by the installing user (-UserSid). The
           elevated UI and the elevated sign-in task run binaries and
           scripts from this folder, so a user-writable install is a
           silent elevation path. The .previous snapshot is protected
           the same way. -SkipAclHardening opts out.

    Rollback: scripts\rollback-openrgb2.ps1 restores the previous
    binaries from <InstallDir>.previous (manifest-verified).

    Exit codes: 0 updated, 1 failure (refused before any change, or
    interrupted after the snapshot -- re-run to resume, or roll back).

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

    # Skip SHA-256 verification of the incoming package and of the
    # applied files (documented escape hatch -- never silent).
    [switch]$SkipVerify,

    # Elevated runs harden the install ACL (step 7). This opts out.
    [switch]$SkipAclHardening,

    # SID of the user who keeps write access to state\ and
    # profiles\local\ after hardening. Default: the install dir's
    # owner when that is not Administrators/SYSTEM, else the user
    # already granted write on state\, else the current user.
    [string]$UserSid
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

# User-state roots preserved across update AND rollback. Relative
# paths, forward or back slashes, compared case-insensitively (NTFS).
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

# A manifest path must stay inside the tree it describes: relative, no
# drive/UNC/rooted form, no empty/'.'/'..' segment, no ADS colon or
# wildcard, no trailing dot/space (Win32 strips those -> aliasing).
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

# The interrupted-update marker (see step 3). An unreadable marker
# still means "interrupted" -- never a reason to retake the snapshot.
function Read-UpdateMarker([string]$InstallDir) {
    $m = Join-Path $InstallDir ".update-in-progress.json"
    if(-not (Test-Path -LiteralPath $m)) { return $null }
    try { return (Get-Content -LiteralPath $m -Raw | ConvertFrom-Json) }
    catch { return [pscustomobject]@{ from_version = ""; to_files = @() } }
}

# SHA-256-verify every file a manifest lists. Returns mismatch list.
function Test-ManifestFiles([string]$dir, $manifest, [switch]$SkipPreserved) {
    $bad = @()
    foreach($f in $manifest.files) {
        if($SkipPreserved -and (Test-PreservedPath $f.path)) { continue }
        $p = Join-Path $dir ($f.path -replace '/','\')
        if(-not (Test-Path -LiteralPath $p)) { $bad += "missing: $($f.path)"; continue }
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower()
        if($h -ne $f.sha256) { $bad += "hash mismatch: $($f.path)" }
    }
    return ,$bad
}

# Install executables that cannot be opened for exclusive write: a
# loaded image (any integrity level, any user) holds a sharing lock,
# so this catches a running stack even when its processes are
# invisible to Get-Process path checks. Access-denied is reported
# separately: that is a protected (admin-only) install, not a lock.
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

# Non-null when ANY managed process may be running -- engine, UI, or
# pinned backend (a running UI locks openrgb2-ui.exe, so it must
# refuse too). A same-named process counts as RUNNING when its image
# path is this install's, and as UNVERIFIABLE when its image path
# cannot be read from this shell (an elevated stack seen from a
# non-elevated shell -- refuse rather than guess, like
# stop-openrgb2.ps1's Resolve-OwnPid). -ProcessList is an offline-test
# seam (objects with ProcessName, Id, Path).
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

function Test-IsElevated {
    try {
        $p = New-Object Security.Principal.WindowsPrincipal(
                 [Security.Principal.WindowsIdentity]::GetCurrent())
        return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { return $false }
}

# Who keeps write access to state\ + profiles\local\ after hardening.
function Resolve-InstallUserSid([string]$InstallDir) {
    $privileged = @("S-1-5-32-544","S-1-5-18","S-1-5-19","S-1-5-20","S-1-3-0")
    $sidType = [Security.Principal.SecurityIdentifier]
    try {
        $owner = (Get-Acl -LiteralPath $InstallDir).GetOwner($sidType).Value
        if($owner -and $privileged -notcontains $owner -and
           -not $owner.StartsWith("S-1-5-80-")) { return $owner }
    } catch { }
    try {
        $stateDir = Join-Path $InstallDir "state"
        if(Test-Path -LiteralPath $stateDir) {
            foreach($r in (Get-Acl -LiteralPath $stateDir).GetAccessRules(
                               $true, $false, $sidType)) {
                $sid = $r.IdentityReference.Value
                if([string]$r.AccessControlType -eq "Allow" -and
                   $privileged -notcontains $sid -and
                   $sid -ne "S-1-5-32-545" -and
                   ($r.FileSystemRights -band
                    [Security.AccessControl.FileSystemRights]::WriteData)) {
                    return $sid
                }
            }
        }
    } catch { }
    return [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
}

function Invoke-Icacls([string[]]$IcaclsArgs) {
    $out = & icacls.exe @IcaclsArgs 2>&1
    if($LASTEXITCODE -ne 0) {
        throw "icacls $($IcaclsArgs -join ' ') failed (exit $LASTEXITCODE): $($out -join ' ')"
    }
}

# Harden a tree for an elevated consumer: owner Administrators (an
# owner keeps WRITE_DAC, so the user must not own it), inheritance
# broken, Administrators + SYSTEM full control, Users read/execute.
# $WritableRel dirs get Modify for $UserSid (user state). state\backend
# is the backend's working + config directory, so its plugins\ subdir
# is pinned admin-only: the vendored OpenRGB loads plugin DLLs from
# <config>\plugins whenever its plugin manager runs.
function Set-InstallAcl([string]$Dir, [string]$UserSid, [string[]]$WritableRel) {
    $admins = "*S-1-5-32-544"; $system = "*S-1-5-18"; $users = "*S-1-5-32-545"
    foreach($w in $WritableRel) {
        New-Item -ItemType Directory -Force -Path (Join-Path $Dir $w) | Out-Null
    }
    Invoke-Icacls @($Dir, "/setowner", $admins, "/T", "/Q")
    Invoke-Icacls @($Dir, "/reset", "/T", "/Q")
    Invoke-Icacls @($Dir, "/inheritance:r", "/grant:r",
                    "${admins}:(OI)(CI)F", "${system}:(OI)(CI)F",
                    "${users}:(OI)(CI)RX", "/Q")
    foreach($w in $WritableRel) {
        Invoke-Icacls @((Join-Path $Dir $w), "/grant", "*${UserSid}:(OI)(CI)M", "/Q")
    }
    if($WritableRel -contains "state") {
        $plug = Join-Path $Dir "state\backend\plugins"
        New-Item -ItemType Directory -Force -Path $plug | Out-Null
        Invoke-Icacls @($plug, "/setowner", $admins, "/T", "/Q")
        Invoke-Icacls @($plug, "/inheritance:r", "/grant:r",
                        "${admins}:(OI)(CI)F", "${system}:(OI)(CI)F",
                        "${users}:(OI)(CI)RX", "/Q")
    }
}

function Invoke-PackageUpdate {
    param(
        [string]$PackageDir,
        [string]$InstallDir,
        [bool]$SkipVerify,
        [bool]$SkipAclHardening,
        [string]$UserSid,
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
    $unsafe = Get-UnsafeManifestPaths $newManifest
    if($unsafe.Count -gt 0) {
        Write-Host "ERROR: package manifest lists unsafe paths (rooted / '..' / ADS) -- refusing:" -ForegroundColor Red
        $unsafe | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
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

    #---- 2. refuse when running / unverifiable / locked ---------------
    $running = if($PSBoundParameters.ContainsKey('ProcessList')) {
                   Test-InstallRunning $InstallDir -ProcessList $ProcessList
               } else { Test-InstallRunning $InstallDir }
    if($running) {
        Write-RunningRefusal $running "update"
        return 1
    }

    $oldManifest = Read-Manifest $InstallDir
    if($oldManifest) {
        $unsafe = Get-UnsafeManifestPaths $oldManifest
        if($unsafe.Count -gt 0) {
            Write-Host "ERROR: the INSTALLED manifest lists unsafe paths -- refusing to diff against it: $($unsafe -join ', ')" -ForegroundColor Red
            return 1
        }
    }
    $backupDir   = "$InstallDir.previous"
    $markerPath  = Join-Path $InstallDir ".update-in-progress.json"
    $marker      = Read-UpdateMarker $InstallDir
    $fromVersion = if($oldManifest) { [string]$oldManifest.version } else { "unknown" }
    $toFiles     = @($newManifest.files | ForEach-Object { [string]$_.path })

    #---- 3. snapshot the current install (before ANY change) ----------
    if($marker) {
        # An earlier update was interrupted after its snapshot: that
        # snapshot is the only known-good copy -- never retake it.
        if(-not (Read-Manifest $backupDir)) {
            Write-Host ("ERROR: an interrupted update left $markerPath but " +
                "the snapshot $backupDir is missing or has no manifest -- " +
                "nothing was changed. Restore the install by hand (or copy " +
                "a fresh staged package over it), then delete the marker.") -ForegroundColor Red
            return 1
        }
        if($marker.from_version) { $fromVersion = [string]$marker.from_version }
        foreach($p in @($marker.to_files)) {
            if($p -and (Test-SafeRelPath ([string]$p)) -and
               $toFiles -notcontains [string]$p) {
                $toFiles += [string]$p
            }
        }
        Write-Host ("==> resuming an interrupted update (from $fromVersion) -- " +
            "the snapshot $backupDir is kept untouched (rollback: " +
            "scripts\rollback-openrgb2.ps1)") -ForegroundColor Yellow
    } else {
        $tmpDir = "$backupDir.tmp"
        Write-Host "==> snapshotting current install -> $backupDir" -ForegroundColor Cyan
        if(Test-Path -LiteralPath $tmpDir) { Remove-Item -LiteralPath $tmpDir -Recurse -Force }
        Copy-Item -Recurse -Force -LiteralPath $InstallDir -Destination $tmpDir
        # The old snapshot is replaced only once the new one is whole.
        if(Test-Path -LiteralPath $backupDir) { Remove-Item -LiteralPath $backupDir -Recurse -Force }
        Move-Item -LiteralPath $tmpDir -Destination $backupDir
        if(-not $SkipAclHardening -and (Test-IsElevated)) {
            # Rollback restores from the snapshot -- an elevated
            # install must not keep a user-writable copy of itself.
            try { Set-InstallAcl $backupDir "" @() }
            catch {
                Write-Host "ERROR: could not protect the snapshot ${backupDir}: $($_.Exception.Message) -- nothing in the install was changed" -ForegroundColor Red
                return 1
            }
        }
    }
    [ordered]@{
        utc          = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        from_version = $fromVersion
        to_version   = [string]$newManifest.version
        backup       = $backupDir
        to_files     = $toFiles
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -LiteralPath $markerPath

    try {
        #---- 4. manifest-diff delete: drop files the update removes ---
        $newPaths = @{}
        foreach($f in $newManifest.files) { $newPaths[[string]$f.path] = $true }
        if($oldManifest) {
            foreach($f in $oldManifest.files) {
                if(-not $newPaths.ContainsKey([string]$f.path) -and
                   -not (Test-PreservedPath $f.path)) {
                    $p = Join-Path $InstallDir ($f.path -replace '/','\')
                    if(Test-Path -LiteralPath $p) {
                        Remove-Item -LiteralPath $p -Force
                        Write-Host "  removed stale file: $($f.path)"
                    }
                }
            }
        }

        #---- 5. copy the new package in, preserving user state --------
        Write-Host "==> applying package files" -ForegroundColor Cyan
        $copied = 0
        foreach($f in $newManifest.files) {
            if(Test-PreservedPath $f.path) { continue }   # paranoid: a
            # package never ships preserved paths -- assert-and-skip anyway
            $src = Join-Path $PackageDir ($f.path -replace '/','\')
            $dst = Join-Path $InstallDir ($f.path -replace '/','\')
            if(-not (Test-Path -LiteralPath $src)) { continue }
            $dstDir = Split-Path -Parent $dst
            if(-not (Test-Path -LiteralPath $dstDir)) {
                New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
            }
            Copy-Item -Force -LiteralPath $src -Destination $dst
            ++$copied
        }
        if(-not $SkipVerify) {
            $bad = Test-ManifestFiles $InstallDir $newManifest -SkipPreserved
            if($bad.Count -gt 0) {
                throw ("installed files do not match the package manifest: " +
                       (($bad | Select-Object -First 5) -join '; '))
            }
        }
        # manifest.json itself is not in its own files list -- copy it
        # explicitly and LAST, so the install's manifest only ever
        # describes binaries that are all in place (rollback's diff
        # needs the true new set).
        Copy-Item -Force -LiteralPath (Join-Path $PackageDir "manifest.json") -Destination $InstallDir
        Write-Host "  $copied files applied (+ manifest.json)"
    } catch {
        Write-Host "ERROR: update interrupted: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host ("  The install may be partially updated. The snapshot " +
            "$backupDir is intact and kept: re-run this update to resume " +
            "and finish it, or roll back with " +
            "scripts\rollback-openrgb2.ps1.") -ForegroundColor Red
        return 1
    }

    #---- 6. journal -----------------------------------------------------
    $stateDir = Join-Path $InstallDir "state"
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    $journal = Join-Path $stateDir "updates.json"
    $entries = @()
    if(Test-Path $journal) {
        try {
            # PS5 emits a JSON array as ONE Object[] into the pipe --
            # @() over the RESULT enumerates it; a bare object yields 1.
            $read = Get-Content $journal -Raw | ConvertFrom-Json
            $entries = @($read)
        } catch { $entries = @() }
    }
    $entries += [pscustomobject][ordered]@{
        utc          = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        action       = "update"
        from_version = $fromVersion
        to_version   = $newManifest.version
        backup       = $backupDir
    }
    # -InputObject keeps the journal a JSON ARRAY even with a single
    # entry (piping would unroll it to a bare object -- shape drift).
    (ConvertTo-Json -InputObject $entries -Depth 4) |
        Set-Content -Encoding UTF8 $journal
    Remove-Item -LiteralPath $markerPath -Force

    #---- 7. protect the install from the user it runs elevated for ---
    if(-not $SkipAclHardening -and (Test-IsElevated)) {
        if(-not $UserSid) { $UserSid = Resolve-InstallUserSid $InstallDir }
        Write-Host ("==> hardening install ACL (Administrators/SYSTEM full, " +
            "Users read/execute; state\ + profiles\local\ writable by " +
            "$UserSid)") -ForegroundColor Cyan
        try {
            Set-InstallAcl $InstallDir $UserSid @("state", "profiles\local")
        } catch {
            Write-Host "ERROR: $($newManifest.version) is installed, but hardening the install ACL failed: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "  The install stays user-writable and the Windows sign-in task refuses to enable on it -- re-run this update elevated." -ForegroundColor Red
            return 1
        }
    } elseif(-not $SkipAclHardening) {
        Write-Host ("  note: not elevated -- install ACL left unchanged. The " +
            "managed stack runs elevated from this folder: re-run the update " +
            "from an elevated PowerShell to make it admin-only (the Windows " +
            "sign-in task requires that).") -ForegroundColor Yellow
    }

    Write-Host "UPDATE OK: $($newManifest.version) installed; rollback: scripts\rollback-openrgb2.ps1" -ForegroundColor Green
    return 0
}

if($MyInvocation.InvocationName -ne '.') {
    # An elevated run must not auto-load modules from the user's own
    # (user-writable) Documents module path.
    if(Test-IsElevated) {
        $env:PSModulePath = (Join-Path $env:ProgramFiles "WindowsPowerShell\Modules") +
            ";" + (Join-Path $PSHOME "Modules")
    }
    exit (Invoke-PackageUpdate -PackageDir $PackageDir `
        -InstallDir $InstallDir -SkipVerify $SkipVerify `
        -SkipAclHardening ([bool]$SkipAclHardening) -UserSid $UserSid)
}
