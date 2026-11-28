<#
.SYNOPSIS
    Re-staging over a package that was run in place (audit P-2).
    CTest: package_stage_release_state_test (label packaging).

.DESCRIPTION
    Runs the REAL packaging\windows\stage-release.ps1 (as a separate
    -File process, the way a developer runs it) against throwaway
    fake inputs under %TEMP%: placeholder application/backend/CRT
    files and a fake windeployqt (fake_windeployqt.exe, built by
    CTest) that drops the runtime members the verification pass
    requires. Nothing under build\package is touched.

      S1  a fresh stage succeeds and writes manifest.json
      S2  re-staging a package that was run in place keeps its
          state\ and profiles\local\ byte-identical, and neither is
          listed in the new manifest
      S3  a staging run that fails after the wipe leaves the user
          state in the sibling "<OutDir>.user-state"; the next good
          run puts it back
      S4  an existing -OutDir without manifest.json (not a staged
          package) is refused and left untouched

    Exit code: number of failed checks.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$StageScript,
    [Parameter(Mandatory)][string]$FakeWindeployqt
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
function Sha([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }
function Touch([string]$path, [string]$text = "placeholder") {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllText($path, $text)
}

$Root = Join-Path $env:TEMP ("openrgb2-stagetest-" + [guid]::NewGuid().ToString("N").Substring(0,8))

# Fake inputs: the staging script only copies these, so any bytes do.
$Build   = Join-Path $Root "build"
$Qt      = Join-Path $Root "qt"
$QtBad   = Join-Path $Root "qt-broken"
$Backend = Join-Path $Root "backend-deploy"
$Crt     = Join-Path $Root "crt"
$Out     = Join-Path $Root "out\OpenRGB2-test-win64"
$Holding = "$Out.user-state"

function Invoke-Stage([string]$QtDir, [string]$OutDir) {
    # The child's stderr lines must stay data: under Windows
    # PowerShell 5.1 "2>&1" + "Stop" would turn them into a
    # terminating error here.
    $ErrorActionPreference = "Continue"
    $log = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $StageScript `
        -BuildDir $Build -OutDir $OutDir -QtDir $QtDir `
        -BackendDeployDir $Backend -VcRedistDir $Crt 2>&1
    $rc = $LASTEXITCODE
    return [pscustomobject]@{ Rc = $rc; Log = (($log | ForEach-Object { "$_" }) -join "`n") }
}

try {
    Touch (Join-Path $Build "src\ui\Release\openrgb2-ui.exe")
    Touch (Join-Path $Build "src\engine\Release\openrgb2-engine.exe")
    Touch (Join-Path $Build "tools\engine_ctl\Release\openrgb2-ctl.exe")
    foreach($f in @("OpenRGB.exe", "libusb-1.0.dll", "hidapi-hotplug.dll", "PawnIOLib.dll")) {
        Touch (Join-Path $Backend $f)
    }
    Touch (Join-Path $Crt "vcruntime140.dll")
    New-Item -ItemType Directory -Force (Join-Path $Qt "bin") | Out-Null
    Copy-Item $FakeWindeployqt (Join-Path $Qt "bin\windeployqt.exe")
    Touch (Join-Path $Qt "bin\Qt6Network.dll")
    # A windeployqt that cannot run: staging fails AFTER the wipe.
    Touch (Join-Path $QtBad "bin\windeployqt.exe") ""
    Touch (Join-Path $QtBad "bin\Qt6Network.dll")

    #================================================================#
    Section "S1 fresh stage"
    #================================================================#
    $r = Invoke-Stage $Qt $Out
    Check ($r.Rc -eq 0) "fresh stage exits 0"
    Check (Test-Path (Join-Path $Out "manifest.json")) "manifest.json written"
    if($r.Rc -ne 0) { Write-Host $r.Log }

    #================================================================#
    Section "S2 re-stage keeps state\ and profiles\local\"
    #================================================================#
    # What running the package in place leaves behind.
    $userFiles = @("state\backend\OpenRGB.json", "state\startup.log",
                   "profiles\local\engine-state.json",
                   "profiles\local\workspaces\ws-1\workspace.json")
    $hashes = @{}
    foreach($f in $userFiles) {
        Touch (Join-Path $Out $f) ("user data " + $f)
        $hashes[$f] = Sha (Join-Path $Out $f)
    }
    $r = Invoke-Stage $Qt $Out
    Check ($r.Rc -eq 0) "re-stage over a run-in-place package exits 0"
    if($r.Rc -ne 0) { Write-Host $r.Log }
    foreach($f in $userFiles) {
        $p = Join-Path $Out $f
        Check ((Test-Path $p) -and (Sha $p) -eq $hashes[$f]) "re-stage kept $f byte-identical"
    }
    if(Test-Path (Join-Path $Out "manifest.json")) {
        $m = Get-Content (Join-Path $Out "manifest.json") -Raw | ConvertFrom-Json
        $leaked = @($m.files | Where-Object { $_.path -like "state/*" -or $_.path -like "profiles/local/*" })
        Check ($leaked.Count -eq 0) "user state is not listed in the new manifest"
        Check (@($m.files | Where-Object { $_.path -like ".staging*" }).Count -eq 0) `
              "the staging marker is not listed in the manifest"
    } else { Check $false "manifest.json present after re-stage" }
    Check (-not (Test-Path $Holding)) "no holding dir left after a good run"
    Check (@(Get-ChildItem -LiteralPath $Out -Force -Filter ".staging*").Count -eq 0) `
          "no staging marker left after a good run"

    #================================================================#
    Section "S3 failed staging keeps the state aside, next run restores it"
    #================================================================#
    $r = Invoke-Stage $QtBad $Out
    Check ($r.Rc -ne 0) "staging with a broken windeployqt fails"
    foreach($f in $userFiles) {
        $p = Join-Path $Holding $f
        Check ((Test-Path $p) -and (Sha $p) -eq $hashes[$f]) "failed run kept $f in the holding dir"
    }
    # A second failing run must not lose what the first one set aside.
    $r = Invoke-Stage $QtBad $Out
    Check ($r.Rc -ne 0) "second failing run fails too"
    Check ((Test-Path (Join-Path $Holding $userFiles[0]))) "second failing run kept the holding dir"
    $r = Invoke-Stage $Qt $Out
    Check ($r.Rc -eq 0) "next good run exits 0"
    if($r.Rc -ne 0) { Write-Host $r.Log }
    foreach($f in $userFiles) {
        $p = Join-Path $Out $f
        Check ((Test-Path $p) -and (Sha $p) -eq $hashes[$f]) "good run restored $f"
    }
    Check (-not (Test-Path $Holding)) "holding dir removed after the restore"

    #================================================================#
    Section "S4 a directory that is not a staged package is never wiped"
    #================================================================#
    $notPkg = Join-Path $Root "not-a-package"
    Touch (Join-Path $notPkg "precious.txt") "do not delete"
    Touch (Join-Path $notPkg "state\keep.json") "{}"
    $r = Invoke-Stage $Qt $notPkg
    Check ($r.Rc -ne 0) "stage into a non-package directory is refused"
    Check ($r.Log -match "no manifest\.json") "the refusal names the missing manifest.json"
    Check ((Test-Path (Join-Path $notPkg "precious.txt")) -and
           (Test-Path (Join-Path $notPkg "state\keep.json"))) "the directory is left untouched"
    Check (-not (Test-Path "$notPkg.user-state")) "nothing was moved aside"

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
