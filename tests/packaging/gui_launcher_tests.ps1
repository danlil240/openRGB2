param([Parameter(Mandatory)][string]$GuiScript,
      [Parameter(Mandatory)][string]$Vbs)

$ErrorActionPreference = 'Stop'
$root = Join-Path $env:TEMP ('openrgb2 gui launcher ' + [guid]::NewGuid().ToString('N'))
$scripts = Join-Path $root 'scripts'
New-Item -ItemType Directory -Force $scripts | Out-Null

try {
    Copy-Item $GuiScript (Join-Path $scripts 'start-openrgb2-gui.ps1')
    @'
function Invoke-ManagedStart {
    param([string]$InstallDir, [bool]$TakeOver, [bool]$HideWindows,
          [int]$BackendWaitSeconds, [int]$ListenerPidOverride)
    Write-Host "FAKE START: takeover=$TakeOver hidden=$HideWindows install=$InstallDir"
    if(Test-Path (Join-Path $InstallDir 'fail.marker')) { return 3 }
    if(Test-Path (Join-Path $InstallDir 'elevated.marker')) {
        Write-Host "UNVERIFIABLE: listener image path unreadable"
        return 4
    }
    if((Test-Path (Join-Path $InstallDir 'conflict.marker')) -and -not $TakeOver) {
        Write-Host "  ownership CONFLICT: iCUE (pid 4242, iCUE.exe) -- competing RGB owner" -ForegroundColor Red
        Write-Host ""
        Write-Host "OWNERSHIP CONFLICT -- OpenRGB2 will not take over silently." -ForegroundColor Red
        return 3
    }
    return 0
}
'@ | Set-Content -Encoding UTF8 (Join-Path $scripts 'start-openrgb2.ps1')

    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts 'start-openrgb2-gui.ps1') -NoDialog
    if($LASTEXITCODE -ne 0) { throw "successful fake startup exited $LASTEXITCODE" }
    $log = Join-Path $root 'state\startup.log'
    if(-not (Test-Path $log)) { throw 'startup log was not created' }
    $contents = Get-Content $log -Raw
    if($contents -notmatch 'FAKE START: takeover=False hidden=True' -or
       $contents -match 'takeover=True' -or
       $contents -notmatch [regex]::Escape("install=$root")) {
        throw 'GUI startup must start WITHOUT takeover, hidden, in the install dir'
    }
    Write-Host 'PASS hidden GUI startup runs without -TakeOver and writes a log'

    # Conflicts + no confirmation: never retried with -TakeOver.
    Remove-Item $log
    Set-Content -Encoding ASCII (Join-Path $root 'conflict.marker') 'c'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts 'start-openrgb2-gui.ps1') -NoDialog
    if($LASTEXITCODE -ne 3) { throw "unconfirmed conflict exited $LASTEXITCODE instead of 3" }
    $contents = Get-Content $log -Raw
    if($contents -match 'takeover=True') { throw 'conflict without confirmation took over' }
    if($contents -notmatch 'takeover declined') { throw 'declined takeover not logged' }
    Write-Host 'PASS competing owners without confirmation never take over'

    # Conflicts + confirmed: exactly one retry with -TakeOver.
    Remove-Item $log
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts 'start-openrgb2-gui.ps1') -NoDialog -TakeOverAnswer Yes
    if($LASTEXITCODE -ne 0) { throw "confirmed takeover exited $LASTEXITCODE" }
    $contents = Get-Content $log -Raw
    if($contents -notmatch '(?s)takeover=False.*ownership CONFLICT: iCUE.*takeover confirmed.*takeover=True') {
        throw 'confirmed takeover did not scan first, list the competitor, then retry with -TakeOver'
    }
    Write-Host 'PASS confirmed takeover retries with -TakeOver after listing competitors'
    Remove-Item (Join-Path $root 'conflict.marker')

    # Exit 4 (unverifiable elevated stack) is its own case, never retried.
    Remove-Item $log
    Set-Content -Encoding ASCII (Join-Path $root 'elevated.marker') 'e'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts 'start-openrgb2-gui.ps1') -NoDialog -TakeOverAnswer Yes
    if($LASTEXITCODE -ne 4) { throw "unverifiable stack exited $LASTEXITCODE instead of 4" }
    if((Get-Content $log -Raw) -match 'takeover=True') { throw 'exit 4 must not retry with -TakeOver' }
    Write-Host 'PASS unverifiable elevated stack (exit 4) is reported, not taken over'
    Remove-Item (Join-Path $root 'elevated.marker')

    Set-Content -Encoding ASCII (Join-Path $root 'fail.marker') 'fail'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts 'start-openrgb2-gui.ps1') -NoDialog
    if($LASTEXITCODE -ne 3) { throw "failed fake startup exited $LASTEXITCODE instead of 3" }
    Write-Host 'PASS failed startup preserves its exit code without opening a dialog in test mode'

    Remove-Item (Join-Path $root 'fail.marker')
    Remove-Item $log
    Copy-Item $Vbs (Join-Path $root 'OpenRGB2.vbs')
    & cscript.exe //nologo (Join-Path $root 'OpenRGB2.vbs')
    if($LASTEXITCODE -ne 0) { throw "VBScript entry exited $LASTEXITCODE" }
    $deadline = (Get-Date).AddSeconds(10)
    while((Get-Date) -lt $deadline) {
        if(Test-Path $log) {
            try {
                $handle = [System.IO.File]::Open($log, 'Open', 'ReadWrite', 'None')
                $handle.Close()
                break
            } catch [System.IO.IOException] {}
        }
        Start-Sleep -Milliseconds 100
    }
    if(-not (Test-Path $log) -or (Get-Content $log -Raw) -notmatch 'FAKE START') {
        throw 'VBScript did not complete the hidden PowerShell entry'
    }
    Write-Host 'PASS VBScript icon entry starts the hidden GUI wrapper'
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
