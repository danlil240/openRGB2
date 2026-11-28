<#
.SYNOPSIS
    stage-release.ps1 -VerifyOnly checks the files against
    manifest.json (audit P-14). CTest: package_stage_verify_test
    (label packaging).

.DESCRIPTION
    Builds a throwaway staged tree under %TEMP% that satisfies the
    structural checks (required members, qml\, presets, no forbidden
    components) with a manifest.json listing every file's SHA-256,
    then runs the REAL script with -VerifyOnly as a -File process:

      V1  an intact tree verifies (exit 0)
      V2  a listed file whose bytes changed fails
      V3  a listed file that is missing fails
      V4  a file the manifest does not list fails (planted DLL)
      V5  a manifest path that escapes the package fails

    Exit code: number of failed checks.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$StageScript)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$script:Pass = 0
$script:Fail = 0
function Check([bool]$cond, [string]$name) {
    if($cond) { $script:Pass++; Write-Host "  PASS $name" }
    else      { $script:Fail++; Write-Host "  FAIL $name" -ForegroundColor Red }
}
function Touch([string]$path, [string]$text = "placeholder") {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllText($path, $text)
}

$Root = Join-Path $env:TEMP ("openrgb2-verifytest-" + [guid]::NewGuid().ToString("N").Substring(0,8))
$Pkg  = Join-Path $Root "OpenRGB2-test-win64"
$Qt   = Join-Path $Root "qt"     # no Qt6Quick3D.dll: the 3D check is skipped

function New-Manifest {
    $files = @()
    foreach($f in @(Get-ChildItem -LiteralPath $Pkg -Recurse -File |
                    Where-Object { $_.Name -ne "manifest.json" })) {
        $files += [ordered]@{
            path   = ($f.FullName.Substring($Pkg.Length).TrimStart('\') -replace '\\','/')
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLower()
            bytes  = $f.Length
            origin = "test"
        }
    }
    [ordered]@{ package = "OpenRGB2"; version = "9.9.9"; files = $files } |
        ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $Pkg "manifest.json")
}
function Invoke-Verify {
    $ErrorActionPreference = "Continue"     # child stderr stays data
    $log = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $StageScript `
        -VerifyOnly -OutDir $Pkg -BuildDir $Root -QtDir $Qt 2>&1
    return [pscustomobject]@{ Rc = $LASTEXITCODE
                              Log = (($log | ForEach-Object { "$_" }) -join "`n") }
}

try {
    New-Item -ItemType Directory -Force (Join-Path $Qt "bin") | Out-Null
    foreach($f in @("openrgb2-ui.exe", "openrgb2-engine.exe", "openrgb2-ctl.exe",
                    "Qt6Core.dll", "Qt6Network.dll", "Qt6QuickControls2.dll",
                    "platforms\qwindows.dll", "backend\OpenRGB.exe",
                    "backend\Qt6Widgets.dll", "backend\platforms\qwindows.dll",
                    "backend\libusb-1.0.dll", "backend\hidapi-hotplug.dll",
                    "backend\PawnIOLib.dll", "backend\payload.bin",
                    "presets\devices\d.json", "presets\devices\d2.json",
                    "presets\effects\e.json", "presets\effects\e2.json", "NOTICES.txt", "licenses\GPL-2.0.txt",
                    "scripts\start-openrgb2.ps1", "scripts\start-openrgb2-gui.ps1",
                    "scripts\startup-task.ps1", "scripts\stop-openrgb2.ps1",
                    "scripts\update-openrgb2.ps1", "scripts\rollback-openrgb2.ps1",
                    "scripts\create-desktop-shortcut.ps1", "OpenRGB2.cmd",
                    "OpenRGB2.vbs", "OpenRGB2.ico", "INSTALL.md", "RELEASE.md",
                    "qml\QtQuick\qmldir")) {
        Touch (Join-Path $Pkg $f) ("content of " + $f)
    }
    New-Manifest

    $r = Invoke-Verify
    Check ($r.Rc -eq 0) "V1 intact staged tree verifies"
    if($r.Rc -ne 0) { Write-Host $r.Log }

    $target = Join-Path $Pkg "backend\libusb-1.0.dll"
    [IO.File]::WriteAllText($target, "tampered")
    $r = Invoke-Verify
    Check ($r.Rc -ne 0) "V2 a listed file with changed bytes fails"
    Check ($r.Log -match [regex]::Escape("backend/libusb-1.0.dll")) "V2 the report names the file"
    [IO.File]::WriteAllText($target, "content of backend\libusb-1.0.dll")
    Check ((Invoke-Verify).Rc -eq 0) "restored file verifies again"

    # (a listed payload the structural checks do not name)
    $gone = Join-Path $Pkg "backend\payload.bin"
    Move-Item -LiteralPath $gone -Destination "$Root\payload.bin.bak"
    $r = Invoke-Verify
    Check ($r.Rc -ne 0) "V3 a listed file that is missing fails"
    Check ($r.Log -match [regex]::Escape("backend/payload.bin")) "V3 the report names the missing file"
    Move-Item -LiteralPath "$Root\payload.bin.bak" -Destination $gone

    $planted = Join-Path $Pkg "backend\version.dll"
    Touch $planted "planted"
    $r = Invoke-Verify
    Check ($r.Rc -ne 0) "V4 a file the manifest does not list fails"
    Check ($r.Log -match [regex]::Escape("backend/version.dll")) "V4 the report names the planted file"
    Remove-Item -LiteralPath $planted

    $m = Get-Content (Join-Path $Pkg "manifest.json") -Raw | ConvertFrom-Json
    $m.files += [pscustomobject]@{ path = "../outside.dll"; sha256 = "00"; bytes = 0; origin = "test" }
    $m | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $Pkg "manifest.json")
    $r = Invoke-Verify
    Check ($r.Rc -ne 0) "V5 a manifest path outside the package fails"
} catch {
    $script:Fail++
    Write-Host "  FAIL unhandled exception: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "    at $($_.InvocationInfo.PositionMessage)" -ForegroundColor Red
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $Root
}

Write-Host ""
Write-Host ("RESULT: {0} passed, {1} failed" -f $script:Pass, $script:Fail)
exit $script:Fail
