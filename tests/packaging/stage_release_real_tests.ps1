<#
.SYNOPSIS
    The REAL stage-release.ps1 against the REAL build tree, Qt kit
    and pinned backend (audit P-13 step 4). CTest:
    package_stage_release_test (label packaging).

.DESCRIPTION
    Stages into a throwaway -OutDir under %TEMP% from:
      * this CMake build tree's application exes (-BuildDir,
        -Configuration),
      * the real windeployqt of the Qt kit the build used (-QtDir),
      * the pinned vendored backend (OpenRGB.exe), found the way
        stage-release.ps1 finds it: $env:OPENRGB2_BACKEND_DIR, then
        vendor\OpenRGB\"OpenRGB Windows 64-bit", then
        vendor\OpenRGB\release.

    When no backend OpenRGB.exe exists (a checkout whose backend was
    never built) or the configuration is Debug (a debug Qt runtime is
    a forbidden package member), the test prints SKIP and exits 77,
    which CTest reports as skipped (SKIP_RETURN_CODE 77).

      R1  staging exits 0, writes manifest.json, clears the
          .staging-incomplete marker
      R2  manifest: identity, configuration, backend pin, every
          staged file listed once with its SHA-256 and size
      R3  layout: application exes byte-identical to the build
          inputs, backend\OpenRGB.exe identical to the pinned one,
          scripts, launchers, presets, licenses
      R4  windeployqt output: Qt runtime + platform plugin + QML
          modules for the UI, Qt runtime for the backend, no debug
          Qt DLLs, Quick3D whole or absent
      R5  no private path (the user profile) in manifest.json
      R6  -VerifyOnly passes on the fresh tree
      R7  -VerifyOnly fails once a staged file changes

    Home for later packaging regressions on a real tree (P-2 state
    preservation, P-14 -VerifyOnly hashes).

    Exit code: number of failed checks (77 = skipped).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$StageScript,
    [Parameter(Mandatory)][string]$BuildDir,
    [string]$Configuration = "Release",
    [string]$QtDir
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$script:Pass = 0
$script:Fail = 0
function Check([bool]$cond, [string]$name) {
    if($cond) { $script:Pass++; Write-Host "  PASS $name" }
    else      { $script:Fail++; Write-Host "  FAIL $name" -ForegroundColor Red }
}
function Section([string]$name) { Write-Host "== $name" -ForegroundColor Cyan }
function Sha([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }
function Skip([string]$why) {
    Write-Host "SKIP: $why"
    exit 77
}

$RepoRoot = (Resolve-Path (Join-Path (Split-Path -Parent $StageScript) "..\..")).Path
$BuildDir = (Resolve-Path $BuildDir).Path

#--------------------------------------------------------------------#
# Preconditions -> skip, never fail                                   #
#--------------------------------------------------------------------#
if($Configuration -eq "Debug") {
    Skip "configuration Debug: a package is staged from a release-runtime build"
}
$backendDir = $null
foreach($cand in @($env:OPENRGB2_BACKEND_DIR,
                   (Join-Path $RepoRoot "vendor\OpenRGB\OpenRGB Windows 64-bit"),
                   (Join-Path $RepoRoot "vendor\OpenRGB\release"))) {
    if($cand -and (Test-Path -LiteralPath (Join-Path $cand "OpenRGB.exe"))) {
        $backendDir = (Resolve-Path -LiteralPath $cand).Path
        break
    }
}
if(-not $backendDir) {
    Skip ("no built backend (OpenRGB.exe) in OPENRGB2_BACKEND_DIR or " +
          "vendor\OpenRGB -- build it with vendor\OpenRGB\scripts\build-windows.bat")
}
if(-not $QtDir) {
    $QtDir = if($env:OPENRGB2_QT_DIR) { $env:OPENRGB2_QT_DIR } else { "C:\Qt\6.8.3\msvc2022_64" }
}
if(-not (Test-Path -LiteralPath (Join-Path $QtDir "bin\windeployqt.exe"))) {
    Skip "no windeployqt under $QtDir"
}
$inputs = [ordered]@{
    "openrgb2-ui.exe"     = Join-Path $BuildDir "src\ui\$Configuration\openrgb2-ui.exe"
    "openrgb2-engine.exe" = Join-Path $BuildDir "src\engine\$Configuration\openrgb2-engine.exe"
    "openrgb2-ctl.exe"    = Join-Path $BuildDir "tools\engine_ctl\$Configuration\openrgb2-ctl.exe"
}
foreach($k in $inputs.Keys) {
    if(-not (Test-Path -LiteralPath $inputs[$k])) {
        Skip "$k is not built for $Configuration ($($inputs[$k]))"
    }
}
Write-Host "backend: $backendDir"
Write-Host "qt:      $QtDir"

$Root = Join-Path $env:TEMP ("openrgb2-stagereal-" + [guid]::NewGuid().ToString("N").Substring(0,8))
$Out  = Join-Path $Root "OpenRGB2-test-win64"

function Invoke-Stage([string[]]$Extra) {
    # Child stderr stays data (Windows PowerShell 5.1 + "Stop" would
    # turn a redirected stderr line into a terminating error here).
    $ErrorActionPreference = "Continue"
    $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $StageScript,
              "-BuildDir", $BuildDir, "-OutDir", $Out,
              "-Configuration", $Configuration, "-QtDir", $QtDir,
              "-BackendDeployDir", $backendDir) + $Extra
    $log = & powershell.exe @argv 2>&1
    $rc = $LASTEXITCODE
    return [pscustomobject]@{ Rc = $rc; Log = (($log | ForEach-Object { "$_" }) -join "`n") }
}

try {
    #================================================================#
    Section "R1 stage"
    #================================================================#
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r = Invoke-Stage @()
    Write-Host ("  staged in {0:n1} s" -f $sw.Elapsed.TotalSeconds)
    Check ($r.Rc -eq 0) "stage-release.ps1 exits 0"
    if($r.Rc -ne 0) {
        Write-Host ($r.Log | Out-String -Width 4096)
        throw "staging failed -- the remaining checks need a staged tree"
    }
    Check (Test-Path -LiteralPath (Join-Path $Out "manifest.json")) "manifest.json written"
    Check (-not (Test-Path -LiteralPath (Join-Path $Out ".staging-incomplete"))) "staging marker cleared"

    #================================================================#
    Section "R2 manifest"
    #================================================================#
    $manifestRaw = Get-Content -Raw -LiteralPath (Join-Path $Out "manifest.json")
    $m = $manifestRaw | ConvertFrom-Json
    $cmakeTop = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot "CMakeLists.txt")
    $version = if($cmakeTop -match "project\s*\(\s*OpenRGB2\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)") { $Matches[1] } else { "?" }
    Check ($m.package -eq "OpenRGB2") "package name"
    Check ($m.version -eq $version) "version $version from CMakeLists.txt"
    Check ($m.platform -eq "windows-x64") "platform"
    Check ($m.source.configuration -eq $Configuration) "configuration recorded"
    Check ($m.source.backend_pin -match '^vendor/OpenRGB @ [0-9a-f]{40} \(') "backend pin from the provenance record"
    Check (@($m.exclusions_verified).Count -gt 0) "exclusions_verified stamped"

    $onDisk = @(Get-ChildItem -LiteralPath $Out -Recurse -File |
        Where-Object { $_.Name -ne "manifest.json" } |
        ForEach-Object { $_.FullName.Substring($Out.Length + 1) -replace '\\','/' })
    $listed = @($m.files | ForEach-Object { $_.path })
    Check ($listed.Count -eq $onDisk.Count) "manifest lists every staged file ($($listed.Count) / $($onDisk.Count))"
    Check (@($listed | Sort-Object -Unique).Count -eq $listed.Count) "no file listed twice"
    $unlisted = @($onDisk | Where-Object { $listed -notcontains $_ })
    Check ($unlisted.Count -eq 0) "no staged file missing from the manifest ($($unlisted -join ', '))"
    $bad = @()
    foreach($f in @($m.files | Where-Object { $_.path -match '\.(exe|json|ps1)$' })) {
        $p = Join-Path $Out ($f.path -replace '/','\')
        if(-not (Test-Path -LiteralPath $p) -or (Sha $p) -ne $f.sha256 -or
           (Get-Item -LiteralPath $p).Length -ne $f.bytes) { $bad += $f.path }
    }
    Check ($bad.Count -eq 0) "manifest SHA-256 + size match for exe/json/ps1 members ($($bad -join ', '))"

    #================================================================#
    Section "R3 layout"
    #================================================================#
    foreach($k in $inputs.Keys) {
        $staged = Join-Path $Out $k
        Check ((Test-Path -LiteralPath $staged) -and ((Sha $staged) -eq (Sha $inputs[$k]))) "$k is the build tree's binary"
    }
    $be = Join-Path $Out "backend\OpenRGB.exe"
    Check ((Test-Path -LiteralPath $be) -and ((Sha $be) -eq (Sha (Join-Path $backendDir "OpenRGB.exe")))) "backend\OpenRGB.exe is the pinned backend"
    foreach($rel in @("scripts\start-openrgb2.ps1", "scripts\stop-openrgb2.ps1",
                      "scripts\update-openrgb2.ps1", "scripts\rollback-openrgb2.ps1",
                      "scripts\startup-task.ps1", "OpenRGB2.cmd", "OpenRGB2.vbs",
                      "OpenRGB2.ico", "INSTALL.md", "RELEASE.md", "NOTICES.txt",
                      "vcruntime140.dll", "backend\vcruntime140.dll",
                      "backend\libusb-1.0.dll", "backend\hidapi-hotplug.dll")) {
        Check (Test-Path -LiteralPath (Join-Path $Out $rel)) "has $rel"
    }
    Check (@(Get-ChildItem -LiteralPath (Join-Path $Out "presets\devices") -Filter *.json).Count -gt 0) "device presets"
    Check (@(Get-ChildItem -LiteralPath (Join-Path $Out "presets\effects") -Filter *.json).Count -gt 0) "effect presets"
    Check (@(Get-ChildItem -LiteralPath (Join-Path $Out "licenses") -File).Count -gt 0) "license texts"
    Check (-not (Test-Path -LiteralPath (Join-Path $Out "profiles\local"))) "no profiles\local"
    Check (-not (Test-Path -LiteralPath (Join-Path $Out "state"))) "no state\"

    #================================================================#
    Section "R4 windeployqt output"
    #================================================================#
    foreach($rel in @("Qt6Core.dll", "Qt6Gui.dll", "Qt6Qml.dll", "Qt6Quick.dll",
                      "Qt6QuickControls2.dll", "Qt6Network.dll",
                      "platforms\qwindows.dll", "qml\QtQuick\Controls",
                      "backend\Qt6Core.dll", "backend\Qt6Widgets.dll",
                      "backend\platforms\qwindows.dll")) {
        Check (Test-Path -LiteralPath (Join-Path $Out $rel)) "deployed $rel"
    }
    $debugQt = @(Get-ChildItem -LiteralPath $Out -Recurse -File -Filter "Qt6*d.dll" |
        Where-Object { $_.Name -ne "Qt6Quick3D.dll" })
    Check ($debugQt.Count -eq 0) "no debug Qt runtime ($(($debugQt | ForEach-Object { $_.Name }) -join ', '))"
    $q3dDll = Test-Path -LiteralPath (Join-Path $Out "Qt6Quick3D.dll")
    $q3dQml = Test-Path -LiteralPath (Join-Path $Out "qml\QtQuick3D")
    Check ($q3dDll -eq $q3dQml) "Quick3D whole or absent (dll=$q3dDll qml=$q3dQml)"

    #================================================================#
    Section "R5 privacy"
    #================================================================#
    $prof = [Environment]::GetFolderPath('UserProfile')
    $profJson = ($prof -replace '\\','\\')
    Check (-not ($manifestRaw.IndexOf($profJson, [StringComparison]::OrdinalIgnoreCase) -ge 0)) "manifest carries no user-profile path"

    #================================================================#
    Section "R6/R7 -VerifyOnly"
    #================================================================#
    $v = Invoke-Stage @("-VerifyOnly")
    Check ($v.Rc -eq 0) "-VerifyOnly passes on the fresh tree"
    if($v.Rc -ne 0) { Write-Host ($v.Log | Out-String -Width 4096) }

    Add-Content -LiteralPath (Join-Path $Out "INSTALL.md") -Value "tampered"
    $t = Invoke-Stage @("-VerifyOnly")
    Check ($t.Rc -ne 0) "-VerifyOnly fails after a staged file changed"
    Check ($t.Log -match 'INSTALL\.md') "the failure names the changed file"
}
catch {
    $script:Fail++
    Write-Host "  FAIL unexpected: $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    if(Test-Path -LiteralPath $Root) {
        Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ("stage_release_real_tests: {0} passed, {1} failed" -f $script:Pass, $script:Fail)
exit $script:Fail
