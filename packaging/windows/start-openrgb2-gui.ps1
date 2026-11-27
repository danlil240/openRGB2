<#
    Hidden Windows launcher entry point. OpenRGB2.vbs starts this script
    without a console. The managed startup remains the single source of
    ownership and process-lifecycle decisions.

    Takeover is never silent: the first start runs WITHOUT -TakeOver.
    When the ownership gate refuses because competing RGB owners are
    running (exit 3 + "OWNERSHIP CONFLICT"), the competitors are listed
    in a Yes/No dialog and the start is retried with -TakeOver only
    after the user confirms. Exit 4 (an elevated stack this launcher
    cannot verify) gets its own message. Every other failure keeps the
    original error dialog.
#>
[CmdletBinding()]
param(
    # Used by the offline launcher test; the desktop entry never passes it.
    [switch]$NoDialog,
    # Offline-test seam: the answer the takeover dialog would get.
    # 'Ask' shows the dialog; with -NoDialog 'Ask' means No (never
    # take over without a confirmation).
    [ValidateSet('Ask', 'Yes', 'No')]
    [string]$TakeOverAnswer = 'Ask'
)

$ErrorActionPreference = 'Stop'
$installDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$stateDir = Join-Path $installDir 'state'
$log = Join-Path $stateDir 'startup.log'
$exitCode = 1
$transcriptStarted = $false
$declined = $false

# Runs one managed start, echoing its host output (so the transcript
# keeps it) while collecting it for the gate decision. The function's
# return value is the last [int] on the pipeline.
function Invoke-GuiManagedStart([bool]$TakeOver) {
    $lines = New-Object System.Collections.Generic.List[string]
    $code = 1
    Invoke-ManagedStart -InstallDir $installDir `
        -TakeOver $TakeOver -HideWindows $true `
        -BackendWaitSeconds 90 -ListenerPidOverride -1 6>&1 |
        ForEach-Object {
            if($_ -is [int]) {
                $code = $_
            } else {
                $text = if($_ -is [System.Management.Automation.InformationRecord]) {
                    [string]$_.MessageData
                } else { [string]$_ }
                $lines.Add($text)
                $text | Out-Host
            }
        }
    return @{ code = [int]$code; lines = $lines.ToArray() }
}

function Confirm-TakeOver([string[]]$Conflicts) {
    if($TakeOverAnswer -eq 'Yes') { return $true }
    if($TakeOverAnswer -eq 'No' -or $NoDialog) { return $false }
    Add-Type -AssemblyName System.Windows.Forms
    $list = ($Conflicts | ForEach-Object { '  ' + $_.Trim() }) -join "`n"
    $message = "Other RGB software is running and controls the same devices:`n`n" +
        "$list`n`n" +
        "Take over anyway? OpenRGB2 never closes these programs - they keep " +
        "running and may fight over the lights. Choose No to close them " +
        "yourself first (recommended)."
    $r = [System.Windows.Forms.MessageBox]::Show(
        $message, 'OpenRGB2 - competing RGB software',
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Warning,
        [System.Windows.Forms.MessageBoxDefaultButton]::Button2)
    return $r -eq [System.Windows.Forms.DialogResult]::Yes
}

try {
    New-Item -ItemType Directory -Force $stateDir | Out-Null
    . (Join-Path $PSScriptRoot 'start-openrgb2.ps1')
    # Rotate before the transcript opens the log: once it is held,
    # Invoke-ManagedStart's own rotation must skip it.
    if(Get-Command Invoke-LogRotation -ErrorAction SilentlyContinue) {
        Invoke-LogRotation $log
    }
    Start-Transcript -Path $log -Append -Force | Out-Null
    $transcriptStarted = $true

    $first = Invoke-GuiManagedStart $false
    $exitCode = $first.code
    if($exitCode -eq 3 -and
       @($first.lines | Where-Object { $_ -match 'OWNERSHIP CONFLICT' }).Count -gt 0) {
        $conflicts = @($first.lines | Where-Object { $_ -match 'ownership CONFLICT:' })
        if(Confirm-TakeOver $conflicts) {
            Write-Host 'GUI: takeover confirmed by the user -- retrying with -TakeOver'
            $exitCode = (Invoke-GuiManagedStart $true).code
        } else {
            Write-Host 'GUI: takeover declined -- nothing started'
            $declined = $true
        }
    }
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    if($transcriptStarted) { Stop-Transcript | Out-Null }
}

if($exitCode -ne 0 -and -not $declined -and -not $NoDialog) {
    Add-Type -AssemblyName System.Windows.Forms
    $message = if($exitCode -eq 4) {
        "OpenRGB2 appears to be already running with administrator rights, " +
        "or a lighting process could not be verified from this " +
        "(non-elevated) launch.`n`nStart OpenRGB2 as administrator.`n`nDetails: $log"
    } else {
        "OpenRGB2 could not start (exit code $exitCode).`n`nDetails: $log"
    }
    [void][System.Windows.Forms.MessageBox]::Show(
        $message, 'OpenRGB2 startup',
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error)
}
exit $exitCode
