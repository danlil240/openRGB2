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
           which stay writable by the installing user (-UserSid, else
           resolved from the folder's ACL; ambiguous = refused before
           any change, never the elevating account). The
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
    # owner when that is a user account, else the one user account
    # granted write on state\ (explicit or inherited; the install dir
    # when state\ is missing). When that is not exactly one account an
    # elevated update REFUSES (nothing changed) and asks for -UserSid --
    # it never falls back to the elevating account.
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

#--------------------------------------------------------------------#
# R-9: backend crash dumps through Windows Error Reporting.            #
# The vendored OpenRGB.exe gets no in-process handler (the engine and  #
# the UI write their own minidumps), so an ELEVATED update sets WER    #
# LocalDumps for OpenRGB.exe: dumps land in <install>\state\crash      #
# (newest 5, the engine's MiniDumpWithThreadInfo |                     #
# MiniDumpWithIndirectlyReferencedMemory flags). The key is per image  #
# NAME, machine-wide (HKLM): a key someone else created is never       #
# overwritten (no OpenRGB2InstallDir owner value), and rollback only   #
# removes the key that names THIS install.                             #
# $script:WerRegistry is the seam: the suite swaps in an in-memory     #
# fake, so tests never touch the real registry.                        #
#--------------------------------------------------------------------#
$script:WerLocalDumpsKey = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\OpenRGB.exe"
$script:WerOwnerValue    = "OpenRGB2InstallDir"
if(-not (Get-Variable -Name WerRegistry -Scope Script -ErrorAction SilentlyContinue)) {
    $script:WerRegistry = @{
        KeyExists = { param($Key) Test-Path -LiteralPath $Key }
        GetValue  = { param($Key, $Name)
            try { (Get-ItemProperty -LiteralPath $Key -Name $Name -ErrorAction Stop).$Name }
            catch { $null } }
        SetValue  = { param($Key, $Name, $Kind, $Value)
            if(-not (Test-Path -LiteralPath $Key)) { New-Item -Path $Key -Force | Out-Null }
            New-ItemProperty -LiteralPath $Key -Name $Name -PropertyType $Kind `
                -Value $Value -Force | Out-Null }
        RemoveKey = { param($Key) Remove-Item -LiteralPath $Key -Recurse -Force }
    }
}

function Get-WerLocalDumpsSpec([string]$InstallDir) {
    return [ordered]@{
        DumpFolder         = @{ Kind = "ExpandString"; Value = (Join-Path $InstallDir "state\crash") }
        DumpCount          = @{ Kind = "DWord";        Value = 5 }
        DumpType           = @{ Kind = "DWord";        Value = 0 }        # custom
        CustomDumpFlags    = @{ Kind = "DWord";        Value = 0x1040 }   # ThreadInfo | IndirectlyReferencedMemory
        OpenRGB2InstallDir = @{ Kind = "String";       Value = $InstallDir }
    }
}

# "set" | "foreign" (someone else's key, left alone) | "failed: <why>"
function Set-WerLocalDumps([string]$InstallDir) {
    $key = $script:WerLocalDumpsKey
    try {
        if(& $script:WerRegistry.KeyExists $key) {
            $owner = & $script:WerRegistry.GetValue $key $script:WerOwnerValue
            if(-not $owner) { return "foreign" }
        }
        New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir "state\crash") | Out-Null
        $spec = Get-WerLocalDumpsSpec $InstallDir
        foreach($name in $spec.Keys) {
            & $script:WerRegistry.SetValue $key $name $spec[$name].Kind $spec[$name].Value
        }
        return "set"
    } catch { return "failed: $($_.Exception.Message)" }
}

# "removed" | "absent" | "foreign" | "other_install" | "failed: <why>"
function Remove-WerLocalDumps([string]$InstallDir) {
    $key = $script:WerLocalDumpsKey
    try {
        if(-not (& $script:WerRegistry.KeyExists $key)) { return "absent" }
        $owner = [string](& $script:WerRegistry.GetValue $key $script:WerOwnerValue)
        if(-not $owner) { return "foreign" }
        $a = $owner.TrimEnd('\','/')
        $b = $InstallDir.TrimEnd('\','/')
        if(-not $a.Equals($b, [StringComparison]::OrdinalIgnoreCase)) { return "other_install" }
        & $script:WerRegistry.RemoveKey $key
        return "removed"
    } catch { return "failed: $($_.Exception.Message)" }
}

# True for a SID that can be "the installing user": a local, domain or
# Entra ID account (S-1-5-21-* / S-1-12-1-*), excluding the built-in
# Administrator account (RID 500) and the well-known domain admin
# groups (512 Domain Admins, 518 Schema Admins, 519 Enterprise Admins).
# Well-known groups and principals (Administrators, SYSTEM, LOCAL/
# NETWORK SERVICE, CREATOR OWNER/GROUP, Users, Authenticated Users,
# Everyone, services S-1-5-80-*, app packages S-1-15-*) never are.
function Test-InstallUserCandidate([string]$Sid) {
    if(-not $Sid) { return $false }
    if(-not ($Sid.StartsWith("S-1-5-21-") -or $Sid.StartsWith("S-1-12-1-"))) {
        return $false
    }
    $rid = $Sid.Substring($Sid.LastIndexOf('-') + 1)
    return @("500","512","518","519") -notcontains $rid
}

# Who keeps write access to state\ + profiles\local\ after hardening:
# the install folder's owner when that is a user account, else the ONE
# user account granted write on state\ (or, when state\ is missing, on
# the install folder) -- explicit AND inherited grants: a fresh
# install's state\ only inherits its user's access from the folder
# above. Returns $null when that is not exactly one account: the
# caller refuses rather than guessing (the elevating account may be a
# different admin than the user the install belongs to). -AclReader
# is the offline-test seam (path -> FileSystemSecurity or $null).
function Resolve-InstallUserSid {
    param(
        [string]$InstallDir,
        [scriptblock]$AclReader = {
            param($p)
            if(Test-Path -LiteralPath $p) { Get-Acl -LiteralPath $p } else { $null }
        }
    )
    $sidType = [Security.Principal.SecurityIdentifier]
    # write/append data, plus GENERIC_ALL / GENERIC_WRITE
    $writeMask = [int64](0x2 -bor 0x4 -bor 0x10000000 -bor 0x40000000)
    $installAcl = $null
    try { $installAcl = & $AclReader $InstallDir } catch { }
    if($installAcl) {
        $owner = $null
        try { $owner = $installAcl.GetOwner($sidType).Value } catch { }
        if(Test-InstallUserCandidate $owner) { return $owner }
    }
    $stateAcl = $null
    try { $stateAcl = & $AclReader ([IO.Path]::Combine($InstallDir, "state")) } catch { }
    foreach($acl in @($stateAcl, $installAcl)) {
        if(-not $acl) { continue }
        $found = @()
        foreach($r in $acl.GetAccessRules($true, $true, $sidType)) {
            if([string]$r.AccessControlType -ne "Allow") { continue }
            if(([int]$r.PropagationFlags -band
                [int][Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
            $sid = $r.IdentityReference.Value
            if(-not (Test-InstallUserCandidate $sid)) { continue }
            if(([int64]$r.FileSystemRights -band $writeMask) -eq 0) { continue }
            if($found -notcontains $sid) { $found += $sid }
        }
        if($found.Count -eq 1) { return $found[0] }
        if($found.Count -gt 1) { return $null }    # ambiguous: never guess
    }
    return $null
}

# -UserSid when given (validated as a SID), else the resolver's
# answer. Returns @{Sid; Error}: Sid $null = refuse ACL hardening.
function Resolve-AclUserSid([string]$InstallDir, [string]$UserSid) {
    if($UserSid) {
        try {
            $sid = (New-Object Security.Principal.SecurityIdentifier($UserSid)).Value
            return [pscustomobject]@{ Sid = $sid; Error = "" }
        } catch {
            return [pscustomobject]@{ Sid = $null
                Error = "-UserSid '$UserSid' is not a valid SID -- nothing was changed." }
        }
    }
    $sid = Resolve-InstallUserSid $InstallDir
    if($sid) { return [pscustomobject]@{ Sid = $sid; Error = "" } }
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    return [pscustomobject]@{ Sid = $null; Error = (
        "cannot tell which user this install belongs to (the folder's " +
        "owner is not a user account and state\ does not grant write " +
        "to exactly one user) -- nothing was changed. This elevated " +
        "update makes the install admin-only and keeps state\ + " +
        "profiles\local\ writable for ONE user; it will not guess, " +
        "because the elevating account may not be that user. Re-run " +
        "with -UserSid <SID of the user who runs OpenRGB2> (from that " +
        "user's non-elevated shell: whoami /user; this elevated " +
        "account is $me), or with -SkipAclHardening.") }
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
    # No trailing separator (as stage-release.ps1 does): the snapshot
    # is "$InstallDir.previous", and "C:\x\OpenRGB2\" + ".previous"
    # lands INSIDE the install -- the update then copied the install
    # into itself (audit P-9).
    $InstallDir = (Resolve-Path $InstallDir).Path.TrimEnd('\','/')
    if(-not $PackageDir) {
        Write-Host "ERROR: -PackageDir (the new staged package) is required" -ForegroundColor Red
        return 1
    }
    $PackageDir = (Resolve-Path $PackageDir).Path.TrimEnd('\','/')

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

    # Whose state\ + profiles\local\ stay writable after step 7 --
    # settled now, while refusing still changes nothing.
    $hardenAcl = (-not $SkipAclHardening) -and (Test-IsElevated)
    if($hardenAcl) {
        $resolved = Resolve-AclUserSid $InstallDir $UserSid
        if(-not $resolved.Sid) {
            Write-Host "ERROR: $($resolved.Error)" -ForegroundColor Red
            return 1
        }
        $UserSid = $resolved.Sid
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
        if($hardenAcl) {
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
    if($hardenAcl) {
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

    #---- 8. backend crash dumps (R-9, WER LocalDumps) -----------------
    # Best effort: the update itself already succeeded.
    if(Test-IsElevated) {
        $wer = Set-WerLocalDumps $InstallDir
        if($wer -eq "set") {
            Write-Host "==> backend crash dumps: WER LocalDumps for OpenRGB.exe -> state\crash (newest 5)" -ForegroundColor Cyan
        } elseif($wer -eq "foreign") {
            Write-Host ("  note: a WER LocalDumps key for OpenRGB.exe already exists " +
                "and is not OpenRGB2's -- left unchanged (backend dumps go where " +
                "it says)") -ForegroundColor Yellow
        } else {
            Write-Host "  note: backend crash dumps not configured ($wer)" -ForegroundColor Yellow
        }
    } else {
        Write-Host ("  note: not elevated -- backend crash dumps (WER LocalDumps, " +
            "HKLM) not configured; the engine and UI still write their own " +
            "dumps to state\crash") -ForegroundColor Yellow
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
