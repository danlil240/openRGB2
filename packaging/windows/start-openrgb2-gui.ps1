<#
    Hidden Windows launcher entry point. OpenRGB2.vbs starts this script
    without a console. The managed startup remains the single source of
    ownership and process-lifecycle decisions.
#>
[CmdletBinding()]
param(
    # Used by the offline launcher test; the desktop entry never passes it.
    [switch]$NoDialog
)

$ErrorActionPreference = 'Stop'
$installDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$stateDir = Join-Path $installDir 'state'
$log = Join-Path $stateDir 'startup.log'
$exitCode = 1
$transcriptStarted = $false

try {
    New-Item -ItemType Directory -Force $stateDir | Out-Null
    Start-Transcript -Path $log -Append -Force | Out-Null
    $transcriptStarted = $true

    . (Join-Path $PSScriptRoot 'start-openrgb2.ps1')
    $exitCode = [int](Invoke-ManagedStart -InstallDir $installDir `
        -TakeOver $true -HideWindows $true `
        -BackendWaitSeconds 90 -ListenerPidOverride -1)
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    if($transcriptStarted) { Stop-Transcript | Out-Null }
}

if($exitCode -ne 0 -and -not $NoDialog) {
    Add-Type -AssemblyName System.Windows.Forms
    $message = "OpenRGB2 could not start (exit code $exitCode).`n`nDetails: $log"
    [void][System.Windows.Forms.MessageBox]::Show(
        $message, 'OpenRGB2 startup',
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error)
}
exit $exitCode
