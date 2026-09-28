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
    return 0
}
'@ | Set-Content -Encoding UTF8 (Join-Path $scripts 'start-openrgb2.ps1')

    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts 'start-openrgb2-gui.ps1') -NoDialog
    if($LASTEXITCODE -ne 0) { throw "successful fake startup exited $LASTEXITCODE" }
    $log = Join-Path $root 'state\startup.log'
    if(-not (Test-Path $log)) { throw 'startup log was not created' }
    $contents = Get-Content $log -Raw
    if($contents -notmatch 'FAKE START: takeover=True hidden=True' -or
       $contents -notmatch [regex]::Escape("install=$root")) {
        throw 'GUI startup did not pass the approved ownership and window settings'
    }
    Write-Host 'PASS hidden GUI startup passes takeover and writes a log'

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
