param([Parameter(Mandatory)][string]$Launcher)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path $env:TEMP ('openrgb2-launcher-' + [guid]::NewGuid().ToString('N'))
$oldPath = $env:PATH
$oldLog = $env:ORGB2_TEST_LOG

try {
    $sourceDir = Join-Path $testRoot 'packaging\windows'
    $releaseDir = Join-Path $testRoot 'build\package\OpenRGB2-0.1.0-win64'
    $shimDir = Join-Path $testRoot 'shim'
    foreach($dir in @($sourceDir, (Join-Path $releaseDir 'scripts'), $shimDir)) {
        New-Item -ItemType Directory -Force $dir | Out-Null
    }
    Copy-Item $Launcher (Join-Path $sourceDir 'OpenRGB2.cmd')
    Copy-Item $Launcher (Join-Path $releaseDir 'OpenRGB2.cmd')
    Set-Content -Encoding ASCII (Join-Path $sourceDir 'start-openrgb2.ps1') '# source only'
    Set-Content -Encoding ASCII (Join-Path $releaseDir 'scripts\start-openrgb2.ps1') '# staged startup'

    # This executable shim checks the real -File argument while preventing
    # the startup script from touching the backend or physical hardware.
    Set-Content -Encoding ASCII (Join-Path $shimDir 'powershell.cmd') @'
@echo off
if /I not "%~4"=="-File" exit /b 80
if not exist "%~5" exit /b 81
>"%ORGB2_TEST_LOG%" echo FILE=%~f5
>>"%ORGB2_TEST_LOG%" echo ARG=%~6
'@
    $env:PATH = "$shimDir;$oldPath"
    $env:ORGB2_TEST_LOG = Join-Path $testRoot 'result.txt'

    foreach($case in @(
        @{ name='source'; dir=$sourceDir },
        @{ name='release'; dir=$releaseDir }
    )) {
        Remove-Item $env:ORGB2_TEST_LOG -ErrorAction SilentlyContinue
        $launch = Join-Path $case.dir 'OpenRGB2.cmd'
        & $env:ComSpec /d /c ('"' + $launch + '" -NoUI < NUL') | Out-Null
        if(-not (Test-Path $env:ORGB2_TEST_LOG)) {
            throw "$($case.name): launcher did not invoke a valid script"
        }
        $actual = @(Get-Content $env:ORGB2_TEST_LOG)
        $want = (Join-Path $releaseDir 'scripts\start-openrgb2.ps1')
        if($actual[0] -ne "FILE=$want" -or $actual[1] -ne 'ARG=-NoUI') {
            throw "$($case.name): wrong script or argument: $($actual -join ', ')"
        }
        Write-Host "PASS $($case.name) launcher targets staged startup and forwards -NoUI"
    }

    Remove-Item (Join-Path $releaseDir 'scripts\start-openrgb2.ps1')
    Remove-Item $env:ORGB2_TEST_LOG
    $launch = Join-Path $sourceDir 'OpenRGB2.cmd'
    $output = & $env:ComSpec /d /c ('"' + $launch + '" < NUL') 2>&1
    if($LASTEXITCODE -ne 1 -or
       (Test-Path $env:ORGB2_TEST_LOG) -or
       (($output -join "`n") -notmatch 'No staged OpenRGB2 release found')) {
        throw 'source launcher did not explain the missing staged release'
    }
    Write-Host 'PASS source launcher explains missing staged release'
} finally {
    $env:PATH = $oldPath
    $env:ORGB2_TEST_LOG = $oldLog
    if(Test-Path $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
