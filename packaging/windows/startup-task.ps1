<# Manages the current user's elevated OpenRGB2 sign-in task.

   Output (one JSON line): {"enabled":bool,"state":"...","target":"..."}
     state = "enabled"   the task runs THIS install's openrgb2-ui.exe
             "disabled"  no task registered
             "stale"     the user's OpenRGB2-<SID> task exists but runs
                         another path (a moved/deleted install) or
                         its target no longer exists -- "target" names
                         it; disable removes it.

   The task runs at priority 4 (normal). The scheduler default, 7,
   is Below Normal with low I/O priority, and every process the task
   starts (UI, engine, backend) inherits it; a task registered
   without it (older builds) reports "stale" so enabling again
   re-registers it.

   enable refuses (exit 1, message on stderr) when the install folder,
   openrgb2-ui.exe or the scripts\ the elevated UI runs are writable by
   the signed-in user: the task starts that executable with highest
   privileges at every sign-in, so a user-writable install would be a
   silent elevation path. update-openrgb2.ps1 run elevated makes the
   install admin-only. It also refuses when any folder above the
   install (up to the volume root) lets the user rename the install
   aside and plant a look-alike -- e.g. the default
   %LOCALAPPDATA%\Programs\OpenRGB2: the task needs an install under an
   admin-only location such as %ProgramFiles%\OpenRGB2. #>
[CmdletBinding()]
param(
    [ValidateSet('status','enable','disable')]
    [string]$Mode = 'status',
    [string]$InstallDir
)

$ErrorActionPreference = 'Stop'

function Get-OpenRGB2StartupTaskName([string]$Sid) {
    return "OpenRGB2-$Sid"
}

function Get-OpenRGB2StartupTaskTarget($Task) {
    if(-not $Task -or -not $Task.Actions) { return $null }
    $actionPath = [string]$Task.Actions[0].Execute
    if(-not $actionPath) { return $null }
    try { return [IO.Path]::GetFullPath($actionPath) }
    catch { return $actionPath }
}

function Test-OpenRGB2StartupTask($Task, [string]$Exe) {
    if(-not $Task -or -not $Task.Actions -or -not $Task.Principal) {
        return $false
    }
    $actionPath = [string]$Task.Actions[0].Execute
    try { $actionPath = [IO.Path]::GetFullPath($actionPath) }
    catch { return $false }
    $priority = $null
    try { $priority = $Task.Settings.Priority } catch { }
    return $actionPath.Equals($Exe, [StringComparison]::OrdinalIgnoreCase) -and
           ([string]$Task.Principal.RunLevel) -eq 'Highest' -and
           ([string]$Task.Principal.LogonType) -eq 'Interactive' -and
           $null -ne $priority -and [int]$priority -eq 4
}

# The SIDs a non-elevated process of this user acts as: the user, the
# well-known groups every interactive user holds, and the current
# token's groups. Administrators is excluded -- an elevated token
# holding it is not the threat.
function Get-OpenRGB2UserSids([string]$Sid) {
    $mine = @($Sid, 'S-1-1-0', 'S-1-5-11', 'S-1-5-4', 'S-1-5-32-545')
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        $mine += $id.User.Value
        foreach($g in $id.Groups) { $mine += $g.Value }
    } catch { }
    return ,@($mine | Where-Object { $_ -and $_ -ne 'S-1-5-32-544' } | Sort-Object -Unique)
}

# 'owner' when one of $Mine owns $Acl (ownership carries WRITE_DAC),
# 'rights' when an effective (not inherit-only) Allow ACE for one of
# $Mine holds any bit of $Mask, else $null. Deny ACEs are ignored on
# purpose: a stray Allow is reported even if a Deny might cancel it.
function Get-OpenRGB2AclGrant($Acl, [string[]]$Mine, [int64]$Mask) {
    $sidType = [Security.Principal.SecurityIdentifier]
    $owner = $null
    try { $owner = $Acl.GetOwner($sidType).Value } catch { }
    if($owner -and $Mine -contains $owner) { return 'owner' }
    foreach($rule in $Acl.GetAccessRules($true, $true, $sidType)) {
        if([string]$rule.AccessControlType -ne 'Allow') { continue }
        if(([int]$rule.PropagationFlags -band
            [int][Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
        if($Mine -notcontains $rule.IdentityReference.Value) { continue }
        if(([int64]$rule.FileSystemRights -band $Mask) -ne 0) { return 'rights' }
    }
    return $null
}

# Paths the user can modify, given the user's SID. Rights that let a
# non-admin replace or re-permission an object: write/append data,
# delete child, delete, change permissions, take ownership (and the
# generic forms); ownership itself carries WRITE_DAC.
function Get-OpenRGB2UserWritablePaths([string[]]$Paths, [string]$Sid) {
    $mine = Get-OpenRGB2UserSids $Sid
    $writeMask = 0x2 -bor 0x4 -bor 0x40 -bor 0x10000 -bor 0x40000 -bor
                 0x80000 -bor 0x10000000 -bor 0x40000000
    $writable = @()
    foreach($path in $Paths) {
        if(-not (Test-Path -LiteralPath $path)) { continue }
        $grant = Get-OpenRGB2AclGrant (Get-Acl -LiteralPath $path) $mine $writeMask
        if($grant -eq 'owner') { $writable += "$path (owned by the user)" }
        elseif($grant) { $writable += $path }
    }
    return ,$writable
}

# The install folder can be swapped out from ABOVE even when it is
# admin-only itself: whoever can rename it (or any folder above it)
# aside can create a look-alike under the old name, and the sign-in
# task then starts that look-alike's openrgb2-ui.exe with highest
# privileges. Walks every folder from the install dir up to the volume
# root and returns a description for each one the user could replace:
#   - the folder itself grants DELETE, WRITE_DAC, WRITE_OWNER or
#     GENERIC_ALL, or is owned by the user (it can be renamed away, or
#     re-permissioned so it can);
#   - its parent grants FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER or
#     GENERIC_ALL, or is owned by the user (the parent can remove or
#     rename the entry whatever the entry's own ACL says).
# Add-file/add-subdirectory on a parent alone is not reported: the name
# is taken, so nothing can be planted there until the existing entry is
# moved, which needs one of the rights above. The volume root is only
# checked as a parent. -AclReader is the offline-test seam (path ->
# FileSystemSecurity or $null).
function Get-OpenRGB2ReplaceableAncestors {
    param(
        [string]$Install,
        [string]$Sid,
        [scriptblock]$AclReader = { param($p) Get-Acl -LiteralPath $p }
    )
    $mine = Get-OpenRGB2UserSids $Sid
    $selfMask   = 0x10000 -bor 0x40000 -bor 0x80000 -bor 0x10000000
    $parentMask = 0x40 -bor 0x40000 -bor 0x80000 -bor 0x10000000
    $cache = @{}
    $read = {
        param($p)
        if(-not $cache.ContainsKey($p)) {
            $a = $null
            try { $a = & $AclReader $p } catch { $a = $null }
            $cache[$p] = $a
        }
        return $cache[$p]
    }
    $found = @()
    $node = [IO.Path]::GetFullPath($Install).TrimEnd('\')
    while($node) {
        $parent = [IO.Path]::GetDirectoryName($node)
        if(-not $parent) { break }      # $node is the volume root
        $acl = & $read $node
        if($acl) {
            $g = Get-OpenRGB2AclGrant $acl $mine $selfMask
            if($g -eq 'owner') { $found += "$node (owned by you)" }
            elseif($g) { $found += "$node (you may delete or rename it)" }
        }
        $pacl = & $read $parent
        if($pacl) {
            $g = Get-OpenRGB2AclGrant $pacl $mine $parentMask
            if($g -eq 'owner') { $found += "$parent (owned by you)" }
            elseif($g) { $found += "$parent (you may delete or rename what is in it)" }
        }
        $node = $parent.TrimEnd('\')
        if($node.Length -le 2 -and $node.EndsWith(':')) { break }
    }
    return ,@($found | Select-Object -Unique)
}

# What enable checks: the folder the task's executable lives in, the
# executable, and the scripts the elevated UI runs from scripts\.
function Get-OpenRGB2ProtectedPaths([string]$Install) {
    $paths = @($Install, (Join-Path $Install 'openrgb2-ui.exe'))
    $scripts = Join-Path $Install 'scripts'
    if(Test-Path -LiteralPath $scripts) {
        $paths += $scripts
        $paths += @(Get-ChildItem -LiteralPath $scripts -File -ErrorAction SilentlyContinue |
                    ForEach-Object { $_.FullName })
    }
    return ,$paths
}

function Invoke-OpenRGB2StartupTask {
    param(
        [ValidateSet('status','enable','disable')][string]$Mode,
        [string]$InstallDir,
        [string]$Sid,
        [string]$UserName
    )

    $install = [IO.Path]::GetFullPath($InstallDir)
    $exe = Join-Path $install 'openrgb2-ui.exe'
    $name = Get-OpenRGB2StartupTaskName $Sid
    $task = Get-ScheduledTask -TaskName $name -TaskPath '\' `
        -ErrorAction SilentlyContinue

    if($Mode -eq 'enable') {
        if(-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "OpenRGB2 executable is missing: $exe"
        }
        $writable = Get-OpenRGB2UserWritablePaths `
            (Get-OpenRGB2ProtectedPaths $install) $Sid
        $replaceable = Get-OpenRGB2ReplaceableAncestors $install $Sid
        # Literal text (not expanded): the user types it in PowerShell.
        $reinstall = ('The sign-in task needs OpenRGB2 installed under ' +
            'an admin-only location such as %ProgramFiles%\OpenRGB2. ' +
            'Reinstall it there from the source checkout with: ' +
            'scripts\build-all-and-install.ps1 -InstallDir ' +
            '"$env:ProgramFiles\OpenRGB2" (it asks for administrator ' +
            'approval to deploy), then enable this setting from that ' +
            'install.')
        if($writable.Count -gt 0) {
            $fix = if($replaceable.Count -gt 0) { $reinstall }
                   else {
                       'Re-run the install/update from an elevated ' +
                       'PowerShell (update-openrgb2.ps1 makes the ' +
                       'install admin-only), then enable again.'
                   }
            throw ("Refusing to register an elevated sign-in task: " +
                "$($writable[0]) is writable without administrator " +
                "rights, so any program could replace what runs " +
                "elevated at sign-in. $fix")
        }
        if($replaceable.Count -gt 0) {
            throw ("Refusing to register an elevated sign-in task: " +
                "this install is admin-only, but the folder it lives " +
                "in is not -- $($replaceable[0]) -- so any program " +
                "running as you could move the OpenRGB2 folder aside " +
                "and put its own in its place, which the task would " +
                "then start with administrator rights at sign-in. " +
                $reinstall)
        }
        $action = New-ScheduledTaskAction -Execute $exe `
            -Argument '--managed --background' -WorkingDirectory $install
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $UserName
        $principal = New-ScheduledTaskPrincipal -UserId $UserName `
            -LogonType Interactive -RunLevel Highest
        # -Priority 4 = normal: the default 7 starts the whole stack
        # at Below Normal CPU and low I/O priority (audit P-4).
        $settings = New-ScheduledTaskSettingsSet `
            -ExecutionTimeLimit ([timespan]::Zero) `
            -MultipleInstances IgnoreNew `
            -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
            -Priority 4
        $null = Register-ScheduledTask -TaskName $name -TaskPath '\' -Action $action `
            -Trigger $trigger -Principal $principal -Settings $settings -Force
        $task = Get-ScheduledTask -TaskName $name -TaskPath '\' -ErrorAction Stop
        if(-not (Test-OpenRGB2StartupTask $task $exe)) {
            throw 'Scheduled task registration did not match the requested executable and privilege level'
        }
    } elseif($Mode -eq 'disable' -and $task) {
        # The task name is per user (OpenRGB2-<SID>): it is this
        # user's sign-in task whether it runs this install, a moved or
        # deleted one, or another copy -- turning the setting off
        # removes it rather than leaving a stale elevated task behind.
        Unregister-ScheduledTask -TaskName $name -TaskPath '\' -Confirm:$false
        $task = Get-ScheduledTask -TaskName $name -TaskPath '\' `
            -ErrorAction SilentlyContinue
    }

    $enabled = [bool](Test-OpenRGB2StartupTask $task $exe)
    $target = Get-OpenRGB2StartupTaskTarget $task
    $state = if($enabled) { 'enabled' }
             elseif($task) { 'stale' }
             else { 'disabled' }
    return [pscustomobject][ordered]@{
        enabled = $enabled
        state   = $state
        target  = $target
    }
}

# Dot-source safe for the offline fake-scheduler regression suite.
if($MyInvocation.InvocationName -ne '.') {
    try {
        # Runs elevated from the UI: never auto-load modules
        # (ScheduledTasks) from the user's own, user-writable path.
        try {
            $isAdmin = (New-Object Security.Principal.WindowsPrincipal(
                [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
                [Security.Principal.WindowsBuiltInRole]::Administrator)
        } catch { $isAdmin = $false }
        if($isAdmin) {
            $env:PSModulePath = (Join-Path $env:ProgramFiles "WindowsPowerShell\Modules") +
                ";" + (Join-Path $PSHOME "Modules")
        }
        if(-not $InstallDir) {
            $InstallDir = Split-Path -Parent $PSScriptRoot
        }
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $result = Invoke-OpenRGB2StartupTask $Mode $InstallDir `
            $identity.User.Value $identity.Name
        $result | ConvertTo-Json -Compress
    } catch {
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 1
    }
}
