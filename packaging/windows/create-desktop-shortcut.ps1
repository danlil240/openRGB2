<# Creates a one-click shortcut for the current Windows user. #>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [string]$DesktopDir
)

$ErrorActionPreference = 'Stop'
if(-not $InstallDir) { $InstallDir = Split-Path -Parent $PSScriptRoot }
$InstallDir = (Resolve-Path -LiteralPath $InstallDir).Path
if(-not $DesktopDir) { $DesktopDir = [Environment]::GetFolderPath('Desktop') }
$DesktopDir = (Resolve-Path -LiteralPath $DesktopDir).Path

$entry = Join-Path $InstallDir 'openrgb2-ui.exe'
$icon = Join-Path $InstallDir 'OpenRGB2.ico'
foreach($required in @($entry, $icon)) {
    if(-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "OpenRGB2 package is missing $required"
    }
}

$linkPath = Join-Path $DesktopDir 'OpenRGB2.lnk'
$link = (New-Object -ComObject WScript.Shell).CreateShortcut($linkPath)
$link.TargetPath = $entry
$link.Arguments = '--managed'
$link.WorkingDirectory = $InstallDir
$link.IconLocation = $icon + ',0'
$link.Description = 'Open OpenRGB2'
$link.Save()
Write-Host "OpenRGB2 desktop shortcut: $linkPath"
