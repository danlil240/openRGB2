param([Parameter(Mandatory)][string]$Script)

$ErrorActionPreference = 'Stop'
. $Script

$root = Join-Path $env:TEMP ('openrgb2 startup task ' + [guid]::NewGuid().ToString('N'))
$sid = 'S-1-5-21-12345'
$user = 'TESTBOX\Daniel'
$script:registered = $null

function Get-ScheduledTask {
    param([string]$TaskName, [string]$TaskPath, [string]$ErrorAction)
    if($TaskPath -ne '\') { throw 'task lookup escaped the root folder' }
    return $script:registered
}
function New-ScheduledTaskAction {
    param([string]$Execute, [string]$Argument, [string]$WorkingDirectory)
    return [pscustomobject]@{Execute=$Execute;Arguments=$Argument;WorkingDirectory=$WorkingDirectory}
}
function New-ScheduledTaskTrigger {
    param([switch]$AtLogOn, [string]$User)
    if(-not $AtLogOn) { throw 'task is not triggered at logon' }
    return [pscustomobject]@{AtLogOn=$true;User=$User}
}
function New-ScheduledTaskPrincipal {
    param([string]$UserId, [string]$LogonType, [string]$RunLevel)
    return [pscustomobject]@{UserId=$UserId;LogonType=$LogonType;RunLevel=$RunLevel}
}
function New-ScheduledTaskSettingsSet {
    param([timespan]$ExecutionTimeLimit, [string]$MultipleInstances,
          [switch]$AllowStartIfOnBatteries, [switch]$DontStopIfGoingOnBatteries,
          [Nullable[int]]$Priority)
    # Unset -> the scheduler default 7 (below normal), as the real
    # cmdlet leaves it.
    $p = if($null -ne $Priority) { [int]$Priority } else { 7 }
    return [pscustomobject]@{ExecutionTimeLimit=$ExecutionTimeLimit
                             MultipleInstances=$MultipleInstances;Priority=$p}
}
function Register-ScheduledTask {
    param([string]$TaskName, [string]$TaskPath, $Action, $Trigger, $Principal, $Settings,
          [switch]$Force)
    if($TaskPath -ne '\') { throw 'task registration escaped the root folder' }
    $script:registered = [pscustomobject]@{
        TaskName=$TaskName;Actions=@($Action);Triggers=@($Trigger)
        Principal=$Principal;Settings=$Settings
    }
    return $script:registered
}
function Unregister-ScheduledTask {
    param([string]$TaskName, [string]$TaskPath, [switch]$Confirm)
    if($TaskPath -ne '\') { throw 'task removal escaped the root folder' }
    $script:registered = $null
}

try {
    New-Item -ItemType Directory -Force $root | Out-Null
    $exe = Join-Path $root 'openrgb2-ui.exe'
    New-Item -ItemType File $exe | Out-Null
    $name = Get-OpenRGB2StartupTaskName $sid
    if($name -ne "OpenRGB2-$sid") { throw "unexpected task name: $name" }
    $status = Invoke-OpenRGB2StartupTask status $root $sid $user
    if($status.enabled -or $status.state -ne 'disabled' -or $status.target) {
        throw 'startup was enabled without a task'
    }

    # A temp dir is owned (and writable) by the signed-in user: an
    # elevated sign-in task over it would be a silent elevation path.
    $refused = $null
    try { $null = Invoke-OpenRGB2StartupTask enable $root $sid $user }
    catch { $refused = $_.Exception.Message }
    if(-not $refused -or $refused -notmatch 'writable' -or $script:registered) {
        throw "enable over a user-writable install was not refused: $refused"
    }
    Write-Host 'PASS enable refuses a user-writable install'

    # An admin-only install inside a user-controlled folder (the temp
    # dir lives under the user's profile, like the default
    # %LOCALAPPDATA%\Programs\OpenRGB2): the folder can be renamed aside
    # and a look-alike planted, so enable must still refuse -- through
    # the REAL ancestor walk over the real profile ACLs.
    function Get-OpenRGB2UserWritablePaths { return ,@() }
    $refused = $null
    try { $null = Invoke-OpenRGB2StartupTask enable $root $sid $user }
    catch { $refused = $_.Exception.Message }
    if(-not $refused -or $script:registered -or
       $refused -notmatch 'admin-only location' -or
       $refused -notmatch [regex]::Escape('%ProgramFiles%\OpenRGB2') -or
       $refused -notmatch [regex]::Escape('build-all-and-install.ps1 -InstallDir "$env:ProgramFiles\OpenRGB2"')) {
        throw "enable inside a user-controlled parent folder was not refused with the reinstall advice: $refused"
    }
    Write-Host 'PASS enable refuses an install whose parent folder the user controls'

    # Ancestor walk over injected ACLs (real security descriptors, no
    # filesystem): a fake volume Q: whose folders get crafted SDDL.
    $bu = 'S-1-5-32-545'
    function New-TestAcl([string]$Sddl) {
        $a = New-Object Security.AccessControl.DirectorySecurity
        $a.SetSecurityDescriptorSddlForm($Sddl)
        return $a
    }
    # C:\ and C:\Program Files as Windows ships them, and an install
    # hardened by update-openrgb2.ps1.
    $rootSddl = 'O:SYG:SYD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;BU)' +
                '(A;OICIIO;SDGRGWGX;;;AU)(A;;0x4;;;AU)'
    $pfSddl   = 'O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464G:SY' +
                'D:PAI(A;;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)' +
                '(A;;0x1301bf;;;SY)(A;;0x1301bf;;;BA)(A;;0x1200a9;;;BU)(A;OICIIO;GA;;;CO)'
    $instSddl = 'O:BAG:SYD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;BU)'
    function Test-Walk([hashtable]$Acls) {
        $reader = { param($p) if($Acls.ContainsKey($p)) { New-TestAcl $Acls[$p] } else { $null } }.GetNewClosure()
        $r = Get-OpenRGB2ReplaceableAncestors 'Q:\Program Files\OpenRGB2' $sid $reader
        return ,@($r)
    }
    $base = @{ 'Q:\' = $rootSddl; 'Q:\Program Files' = $pfSddl
               'Q:\Program Files\OpenRGB2' = $instSddl }
    $cases = @(
        @{ name = 'Program Files layout (Authenticated Users may only add folders to the root)'
           set = @{}; expect = $null },
        @{ name = 'parent grants the user full control (like %LOCALAPPDATA%\Programs)'
           set = @{ 'Q:\Program Files' = "O:SYG:SYD:PAI(A;OICI;FA;;;$sid)" }
           expect = 'Q:\Program Files (you may delete or rename what is in it)' },
        @{ name = 'parent grants Users FILE_DELETE_CHILD only'
           set = @{ 'Q:\Program Files' = ($pfSddl + '(A;;0x40;;;BU)') }
           expect = 'Q:\Program Files (you may delete or rename what is in it)' },
        @{ name = 'parent grants the user add-subdirectory only (name is taken: harmless)'
           set = @{ 'Q:\Program Files' = ($pfSddl + "(A;;0x4;;;$sid)") }; expect = $null },
        @{ name = 'parent grants delete-child to everyone but inherit-only'
           set = @{ 'Q:\Program Files' = ($pfSddl + '(A;OICIIO;0x40;;;WD)') }; expect = $null },
        @{ name = 'parent is owned by the user'
           set = @{ 'Q:\Program Files' = ($pfSddl -replace '^O:[^G]+G:', "O:${sid}G:") }
           expect = 'Q:\Program Files (owned by you)' },
        @{ name = 'install dir grants the user DELETE'
           set = @{ 'Q:\Program Files\OpenRGB2' = ($instSddl + "(A;;SD;;;$sid)") }
           expect = 'Q:\Program Files\OpenRGB2 (you may delete or rename it)' },
        @{ name = 'grandparent (volume root) grants Users WRITE_DAC'
           set = @{ 'Q:\' = ($rootSddl + '(A;;WD;;;BU)') }
           expect = 'Q:\ (you may delete or rename what is in it)' },
        @{ name = 'grandparent (volume root) grants the user delete-child'
           set = @{ 'Q:\' = ($rootSddl + "(A;;0x40;;;$sid)") }
           expect = 'Q:\ (you may delete or rename what is in it)' }
    )
    foreach($c in $cases) {
        $acls = $base.Clone()
        foreach($k in $c.set.Keys) { $acls[$k] = $c.set[$k] }
        $got = Test-Walk $acls
        if($null -eq $c.expect) {
            if($got.Count -ne 0) { throw "ancestor walk: $($c.name): unexpected $($got -join '; ')" }
        } elseif($got -notcontains $c.expect) {
            throw "ancestor walk: $($c.name): expected '$($c.expect)', got '$($got -join '; ')'"
        }
        Write-Host "PASS ancestor walk: $($c.name)"
    }

    # The rest exercises registration as if the install were protected.
    function Get-OpenRGB2ReplaceableAncestors { return ,@() }

    $enabled = Invoke-OpenRGB2StartupTask enable $root $sid $user
    if(-not $enabled.enabled -or $enabled.state -ne 'enabled' -or
       $enabled.target -ne $exe) { throw 'enable did not report active task' }
    if($script:registered.TaskName -ne $name -or
       $script:registered.Actions[0].Execute -ne $exe -or
       $script:registered.Actions[0].Arguments -ne '--managed --background' -or
       $script:registered.Triggers[0].User -ne $user -or
       $script:registered.Principal.RunLevel -ne 'Highest' -or
       $script:registered.Principal.LogonType -ne 'Interactive' -or
       $script:registered.Settings.ExecutionTimeLimit -ne [timespan]::Zero) {
        throw 'scheduled task does not match elevated sign-in contract'
    }
    # The scheduler default priority 7 is Below Normal (plus low I/O
    # priority), inherited by the whole stack it starts: the task must
    # ask for 4, normal (audit P-4).
    if($script:registered.Settings.Priority -ne 4) {
        throw "sign-in task priority is $($script:registered.Settings.Priority), expected 4 (normal)"
    }
    Write-Host 'PASS sign-in task registered at priority 4 (normal)'
    # A task registered by an older build (default priority 7) for this
    # same executable is not reported enabled: turning the setting on
    # re-registers it at normal priority.
    $script:registered.Settings = [pscustomobject]@{Priority=7}
    $old = Invoke-OpenRGB2StartupTask status $root $sid $user
    if($old.enabled -or $old.state -ne 'stale' -or $old.target -ne $exe) {
        throw "a below-normal task was reported as the current one: $($old | ConvertTo-Json -Compress)"
    }
    $enabled = Invoke-OpenRGB2StartupTask enable $root $sid $user
    if(-not $enabled.enabled -or $script:registered.Settings.Priority -ne 4) {
        throw 'enable did not re-register a below-normal task at priority 4'
    }
    Write-Host 'PASS a below-normal task from an older build is stale; enable re-registers it'
    $disabled = Invoke-OpenRGB2StartupTask disable $root $sid $user
    if($disabled.enabled -or $script:registered) {
        throw 'disable left the task registered'
    }
    $script:registered = [pscustomobject]@{
        TaskName=$name
        Actions=@([pscustomobject]@{Execute='C:\Other\openrgb2-ui.exe'})
        Principal=[pscustomobject]@{RunLevel='Highest';LogonType='Interactive'}
    }
    $stale = Invoke-OpenRGB2StartupTask status $root $sid $user
    if($stale.enabled) {
        throw 'another install was reported as this install'
    }
    if($stale.state -ne 'stale' -or $stale.target -ne 'C:\Other\openrgb2-ui.exe') {
        throw "a task for a moved/other install was not reported stale: $($stale | ConvertTo-Json -Compress)"
    }
    # The user's own OpenRGB2-<SID> task pointing elsewhere (moved or
    # deleted install) is removed by disable -- never left behind.
    $cleared = Invoke-OpenRGB2StartupTask disable $root $sid $user
    if($script:registered -or $cleared.enabled -or $cleared.state -ne 'disabled') {
        throw 'disable left the stale task of a moved install registered'
    }
    Write-Host 'PASS per-user elevated sign-in task enable/status/disable/stale'
} finally {
    if(Test-Path $root) {
        $temp = (Resolve-Path -LiteralPath $env:TEMP).Path.TrimEnd('\')
        $resolved = (Resolve-Path -LiteralPath $root).Path
        if(-not $resolved.StartsWith($temp + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing cleanup outside temp directory: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
