param([Parameter(Mandatory)][string]$Installer)

$ErrorActionPreference = 'Stop'
$root = Join-Path $env:TEMP ('openrgb2 shortcut ' + [guid]::NewGuid().ToString('N'))
$desktop = Join-Path $root 'Desktop'
New-Item -ItemType Directory -Force $desktop | Out-Null

try {
    New-Item -ItemType File (Join-Path $root 'OpenRGB2.vbs') | Out-Null
    New-Item -ItemType File (Join-Path $root 'OpenRGB2.ico') | Out-Null
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Installer -InstallDir $root -DesktopDir $desktop
    if($LASTEXITCODE -ne 0) { throw "shortcut installer exited $LASTEXITCODE" }

    $linkPath = Join-Path $desktop 'OpenRGB2.lnk'
    if(-not (Test-Path $linkPath)) { throw 'desktop shortcut was not created' }
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($linkPath)
    if($link.TargetPath -ne (Join-Path $env:SystemRoot 'System32\wscript.exe')) {
        throw "wrong shortcut target: $($link.TargetPath)"
    }
    if($link.Arguments -ne ('"' + (Join-Path $root 'OpenRGB2.vbs') + '"')) {
        throw "wrong shortcut arguments: $($link.Arguments)"
    }
    if($link.IconLocation -notlike ((Join-Path $root 'OpenRGB2.ico') + '*')) {
        throw "wrong shortcut icon: $($link.IconLocation)"
    }
    Write-Host 'PASS desktop shortcut launches the hidden entry point with the package icon'
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
