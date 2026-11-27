<# Manages the current user's elevated OpenRGB2 sign-in task.

   Output (one JSON line): {"enabled":bool,"state":"...","target":"..."}
     state = "enabled"   the task runs THIS install's openrgb2-ui.exe
             "disabled"  no task registered
             "stale"     the user's OpenRGB2-<SID> task exists but runs
                         another path (a moved/deleted install) or
                         its target no longer exists -- "target" names
                         it; disable removes it.

   enable refuses (exit 1, message on stderr) when the install folder,
   openrgb2-ui.exe or the scripts\ the elevated UI runs are writable by
   the signed-in user: the task starts that executable with highest
   privileges at every sign-in, so a user-writable install would be a
   silent elevation path. update-openrgb2.ps1 run elevated makes the
   install admin-only. #>
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
    return $actionPath.Equals($Exe, [StringComparison]::OrdinalIgnoreCase) -and
           ([string]$Task.Principal.RunLevel) -eq 'Highest' -and
           ([string]$Task.Principal.LogonType) -eq 'Interactive'
}

# Paths the user can modify, given the user's SID. Rights that let a
# non-admin replace or re-permission an object: write/append data,
# delete child, delete, change permissions, take ownership (and the
# generic forms); ownership itself carries WRITE_DAC. Administrators
# is excluded -- an elevated token holding it is not the threat.
function Get-OpenRGB2UserWritablePaths([string[]]$Paths, [string]$Sid) {
    $sidType = [Security.Principal.SecurityIdentifier]
    $mine = @($Sid, 'S-1-1-0', 'S-1-5-11', 'S-1-5-4', 'S-1-5-32-545')
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        $mine += $id.User.Value
        foreach($g in $id.Groups) { $mine += $g.Value }
    } catch { }
    $mine = @($mine | Where-Object { $_ -and $_ -ne 'S-1-5-32-544' } | Sort-Object -Unique)
    $writeMask = 0x2 -bor 0x4 -bor 0x40 -bor 0x10000 -bor 0x40000 -bor
                 0x80000 -bor 0x10000000 -bor 0x40000000
    $writable = @()
    foreach($path in $Paths) {
        if(-not (Test-Path -LiteralPath $path)) { continue }
        $acl = Get-Acl -LiteralPath $path
        $owner = $null
        try { $owner = $acl.GetOwner($sidType).Value } catch { }
        if($owner -and $mine -contains $owner) { $writable += "$path (owned by the user)"; continue }
        foreach($rule in $acl.GetAccessRules($true, $true, $sidType)) {
            if([string]$rule.AccessControlType -ne 'Allow') { continue }
            if(([int]$rule.PropagationFlags -band
                [int][Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
            if($mine -notcontains $rule.IdentityReference.Value) { continue }
            if(([int64]$rule.FileSystemRights -band $writeMask) -ne 0) {
                $writable += $path
                break
            }
        }
    }
    return ,$writable
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
        if($writable.Count -gt 0) {
            throw ("Refusing to register an elevated sign-in task: " +
                "$($writable[0]) is writable without administrator " +
                "rights, so any program could replace what runs " +
                "elevated at sign-in. Re-run the install/update from " +
                "an elevated PowerShell (update-openrgb2.ps1 makes the " +
                "install admin-only), then enable again.")
        }
        $action = New-ScheduledTaskAction -Execute $exe `
            -Argument '--managed --background' -WorkingDirectory $install
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $UserName
        $principal = New-ScheduledTaskPrincipal -UserId $UserName `
            -LogonType Interactive -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet `
            -ExecutionTimeLimit ([timespan]::Zero) `
            -MultipleInstances IgnoreNew `
            -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
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
