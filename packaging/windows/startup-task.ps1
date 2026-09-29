<# Manages the current user's elevated OpenRGB2 sign-in task. #>
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
    } elseif($Mode -eq 'disable' -and (Test-OpenRGB2StartupTask $task $exe)) {
        Unregister-ScheduledTask -TaskName $name -TaskPath '\' -Confirm:$false
        $task = Get-ScheduledTask -TaskName $name -TaskPath '\' `
            -ErrorAction SilentlyContinue
    }

    return [pscustomobject]@{
        enabled = [bool](Test-OpenRGB2StartupTask $task $exe)
    }
}

# Dot-source safe for the offline fake-scheduler regression suite.
if($MyInvocation.InvocationName -ne '.') {
    try {
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
