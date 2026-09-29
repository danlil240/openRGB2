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
          [switch]$AllowStartIfOnBatteries, [switch]$DontStopIfGoingOnBatteries)
    return [pscustomobject]@{ExecutionTimeLimit=$ExecutionTimeLimit;MultipleInstances=$MultipleInstances}
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
    if((Invoke-OpenRGB2StartupTask status $root $sid $user).enabled) {
        throw 'startup was enabled without a task'
    }
    $enabled = Invoke-OpenRGB2StartupTask enable $root $sid $user
    if(-not $enabled.enabled) { throw 'enable did not report active task' }
    if($script:registered.TaskName -ne $name -or
       $script:registered.Actions[0].Execute -ne $exe -or
       $script:registered.Actions[0].Arguments -ne '--managed --background' -or
       $script:registered.Triggers[0].User -ne $user -or
       $script:registered.Principal.RunLevel -ne 'Highest' -or
       $script:registered.Principal.LogonType -ne 'Interactive' -or
       $script:registered.Settings.ExecutionTimeLimit -ne [timespan]::Zero) {
        throw 'scheduled task does not match elevated sign-in contract'
    }
    $disabled = Invoke-OpenRGB2StartupTask disable $root $sid $user
    if($disabled.enabled -or $script:registered) {
        throw 'disable left the task registered'
    }
    $script:registered = [pscustomobject]@{
        TaskName=$name
        Actions=@([pscustomobject]@{Execute='C:\Other\openrgb2-ui.exe'})
        Principal=[pscustomobject]@{RunLevel='Highest';LogonType='Interactive'}
    }
    if((Invoke-OpenRGB2StartupTask status $root $sid $user).enabled) {
        throw 'another install was reported as this install'
    }
    $null = Invoke-OpenRGB2StartupTask disable $root $sid $user
    if(-not $script:registered) {
        throw 'disable removed another install task'
    }
    Write-Host 'PASS per-user elevated sign-in task enable/status/disable'
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
