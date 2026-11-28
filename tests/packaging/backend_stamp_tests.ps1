<#
.SYNOPSIS
    Backend staleness check (audit P-7 step 6). CTest:
    package_backend_stamp_test (label packaging).

.DESCRIPTION
    packaging\windows\backend-stamp.ps1 hashes the backend's sources
    (git ls-files -s of vendor/OpenRGB and src/core/playback plus the
    content of every modified or untracked, non-ignored file there)
    and compares the hash with the stamp the backend recipe step
    writes next to OpenRGB.exe. Driven against throwaway git trees
    (no commits needed: the index is enough):

      S1  the hash is 64 hex chars and stable
      S2  a written stamp reads back fresh
      S3  an unstaged edit to a vendor source -> stale; revert -> fresh
      S4  an untracked vendor source -> stale; an ignored build
          output -> still fresh
      S5  an edit outside the backend sources -> still fresh
      S6  an edit under src/core/playback (compiled into the
          backend) -> stale
      S7  a staged change -> stale
      S8  no stamp -> missing; not a git tree -> unknown
      D1  build-and-deploy.ps1 refuses a stale backend ("backend is
          stale"), exit 1
      D2  ... proceeds with -AllowStaleBackend
      D3  ... rebuilds with -BuildBackend (fake recipe), writes the
          stamp, and the next run passes without a rebuild

    Exit code: number of failed checks.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PkgDir
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$script:Pass = 0
$script:Fail = 0
function Check([bool]$cond, [string]$name) {
    if($cond) { $script:Pass++; Write-Host "  PASS $name" }
    else      { $script:Fail++; Write-Host "  FAIL $name" -ForegroundColor Red }
}

function Invoke-Git([string]$repo, [string[]]$gitArgs) {
    $out = & git.exe -C $repo @gitArgs 2>&1
    if($LASTEXITCODE -ne 0) { throw "git $($gitArgs -join ' ') failed: $out" }
}

function New-FakeRepo([string]$path) {
    New-Item -ItemType Directory -Force (Join-Path $path "vendor\OpenRGB\Controllers") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $path "src\core\playback")          | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $path "src\ui")                     | Out-Null
    Set-Content -Encoding ASCII (Join-Path $path "vendor\OpenRGB\Controllers\a.cpp") "int a() { return 1; }"
    Set-Content -Encoding ASCII (Join-Path $path "vendor\OpenRGB\OpenRGB.pro")       "SOURCES += a.cpp"
    Set-Content -Encoding ASCII (Join-Path $path "vendor\OpenRGB\.gitignore") `
        @("release/", "OpenRGB Windows 64-bit/", "*.obj", "*.stamp", "*.bat", "*.exe")
    Set-Content -Encoding ASCII (Join-Path $path "src\core\playback\PlaybackLoop.cpp") "int p() { return 2; }"
    Set-Content -Encoding ASCII (Join-Path $path "src\ui\main.cpp") "int main() { return 0; }"
    Invoke-Git $path @("init", "-q")
    Invoke-Git $path @("add", "-A")
}

$Root = Join-Path $env:TEMP ("openrgb2-stamptest-" + [guid]::NewGuid().ToString("N").Substring(0,8))
try {
    . (Join-Path $PkgDir "backend-stamp.ps1")

    $repo    = Join-Path $Root "repo"
    $backend = Join-Path $repo "vendor\OpenRGB\OpenRGB Windows 64-bit"
    New-FakeRepo $repo
    New-Item -ItemType Directory -Force $backend | Out-Null
    Set-Content -Encoding ASCII (Join-Path $backend "OpenRGB.exe") "fake"

    #---- S1 stable hash ------------------------------------------------
    $h1 = Get-BackendSourceHash $repo
    $h2 = Get-BackendSourceHash $repo
    Check ($h1 -match '^[0-9a-f]{64}$') "S1 hash is 64 hex chars ($h1)"
    Check ($h1 -eq $h2) "S1 hash is stable"

    #---- S2 written stamp is fresh -------------------------------------
    Write-BackendStamp $repo $backend
    Check (Test-Path (Join-Path $backend $BackendStampName)) "S2 stamp written next to OpenRGB.exe"
    $r = Test-BackendStamp $repo $backend
    Check ($r.State -eq "fresh") "S2 fresh after writing ($($r.State))"

    #---- S3 unstaged edit -> stale, revert -> fresh --------------------
    $a = Join-Path $repo "vendor\OpenRGB\Controllers\a.cpp"
    Set-Content -Encoding ASCII $a "int a() { return 42; }"
    $r = Test-BackendStamp $repo $backend
    Check ($r.State -eq "stale") "S3 unstaged vendor edit is stale ($($r.State))"
    Check ($r.Message -match "stale") "S3 message names it stale: $($r.Message)"
    Set-Content -Encoding ASCII $a "int a() { return 1; }"
    Check ((Test-BackendStamp $repo $backend).State -eq "fresh") "S3 reverted edit is fresh again"

    #---- S4 untracked source -> stale, ignored output -> fresh ----------
    $n = Join-Path $repo "vendor\OpenRGB\Controllers\new.cpp"
    Set-Content -Encoding ASCII $n "int n();"
    Check ((Test-BackendStamp $repo $backend).State -eq "stale") "S4 untracked vendor source is stale"
    Remove-Item $n
    New-Item -ItemType Directory -Force (Join-Path $repo "vendor\OpenRGB\release") | Out-Null
    Set-Content -Encoding ASCII (Join-Path $repo "vendor\OpenRGB\release\a.obj") "obj"
    Check ((Test-BackendStamp $repo $backend).State -eq "fresh") "S4 ignored build output stays fresh"

    #---- S5 outside the backend sources -> fresh ------------------------
    Set-Content -Encoding ASCII (Join-Path $repo "src\ui\main.cpp") "int main() { return 1; }"
    Set-Content -Encoding ASCII (Join-Path $repo "README.md") "docs"
    Check ((Test-BackendStamp $repo $backend).State -eq "fresh") "S5 non-backend edits stay fresh"

    #---- S6 src/core/playback -> stale ----------------------------------
    $p = Join-Path $repo "src\core\playback\PlaybackLoop.cpp"
    Set-Content -Encoding ASCII $p "int p() { return 3; }"
    Check ((Test-BackendStamp $repo $backend).State -eq "stale") "S6 src/core/playback edit is stale"
    Set-Content -Encoding ASCII $p "int p() { return 2; }"
    Check ((Test-BackendStamp $repo $backend).State -eq "fresh") "S6 reverted playback edit is fresh"

    #---- S7 staged change -> stale --------------------------------------
    Set-Content -Encoding ASCII (Join-Path $repo "vendor\OpenRGB\OpenRGB.pro") "SOURCES += a.cpp b.cpp"
    Invoke-Git $repo @("add", "vendor/OpenRGB/OpenRGB.pro")
    Check ((Test-BackendStamp $repo $backend).State -eq "stale") "S7 staged vendor change is stale"
    Write-BackendStamp $repo $backend
    Check ((Test-BackendStamp $repo $backend).State -eq "fresh") "S7 re-stamped tree is fresh"

    #---- S8 missing stamp / not a git tree ------------------------------
    $bare = Join-Path $Root "nostamp"
    New-Item -ItemType Directory -Force $bare | Out-Null
    $r = Test-BackendStamp $repo $bare
    Check ($r.State -eq "missing") "S8 no stamp is missing ($($r.State))"
    $plain = Join-Path $Root "plain"
    New-Item -ItemType Directory -Force (Join-Path $plain "vendor\OpenRGB") | Out-Null
    Check ($null -eq (Get-BackendSourceHash $plain)) "S8 no git tree: no hash"
    Check ((Test-BackendStamp $plain $backend).State -eq "unknown") "S8 no git tree is unknown"

    #---- D: build-and-deploy.ps1 gate -----------------------------------
    # A fake repo carrying the REAL build-and-deploy.ps1 + backend-stamp.ps1,
    # a stub stage-release.ps1 and a fake backend recipe.
    $drepo = Join-Path $Root "deploy"
    New-FakeRepo $drepo
    $dpkg = Join-Path $drepo "packaging\windows"
    New-Item -ItemType Directory -Force $dpkg | Out-Null
    Copy-Item (Join-Path $PkgDir "build-and-deploy.ps1") $dpkg
    Copy-Item (Join-Path $PkgDir "backend-stamp.ps1")    $dpkg
    Set-Content -Encoding ASCII (Join-Path $dpkg "stage-release.ps1") "exit 0"
    Set-Content -Encoding ASCII (Join-Path $drepo "CMakeLists.txt") "project(OpenRGB2 VERSION 0.0.1)"
    New-Item -ItemType Directory -Force (Join-Path $drepo "build") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $drepo "vendor\OpenRGB\scripts") | Out-Null
    $marker = Join-Path $drepo "recipe-ran.txt"
    # The fake recipe: (re)creates the deploy dir like build-windows.bat does.
    Set-Content -Encoding ASCII (Join-Path $drepo "vendor\OpenRGB\scripts\build-windows.bat") @(
        "@echo off",
        "echo ran>>`"$marker`"",
        "if not exist `"OpenRGB Windows 64-bit`" mkdir `"OpenRGB Windows 64-bit`"",
        "echo fake>`"OpenRGB Windows 64-bit\OpenRGB.exe`"",
        "exit /b 0")
    $dbackend = Join-Path $drepo "vendor\OpenRGB\OpenRGB Windows 64-bit"
    New-Item -ItemType Directory -Force $dbackend | Out-Null
    Set-Content -Encoding ASCII (Join-Path $dbackend "OpenRGB.exe") "old"
    Set-Content -Encoding ASCII (Join-Path $drepo "vendor\OpenRGB\Controllers\a.cpp") "int a() { return 7; }"

    $savedBackendDir = $env:OPENRGB2_BACKEND_DIR
    $env:OPENRGB2_BACKEND_DIR = $null
    try {
        $deploy = Join-Path $dpkg "build-and-deploy.ps1"

        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $deploy -SkipBuild 2>&1 |
               Out-String -Width 4096
        Check ($LASTEXITCODE -eq 1) "D1 stale backend fails (exit $LASTEXITCODE)"
        Check ($out -match "backend is stale") "D1 says 'backend is stale'"
        Check (-not (Test-Path $marker)) "D1 does not rebuild without -BuildBackend"

        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $deploy -SkipBuild -AllowStaleBackend 2>&1 |
               Out-String -Width 4096
        Check ($LASTEXITCODE -eq 0) "D2 -AllowStaleBackend proceeds (exit $LASTEXITCODE)"
        Check ($out -match "WARNING: backend is stale") "D2 warns"

        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $deploy -SkipBuild -BuildBackend 2>&1 |
               Out-String -Width 4096
        Check ($LASTEXITCODE -eq 0) "D3 -BuildBackend rebuilds and proceeds (exit $LASTEXITCODE)"
        Check (Test-Path $marker) "D3 the recipe ran"
        Check ((Test-BackendStamp $drepo $dbackend).State -eq "fresh") "D3 stamp written after the rebuild"

        Remove-Item $marker
        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $deploy -SkipBuild -BuildBackend 2>&1 |
               Out-String -Width 4096
        Check ($LASTEXITCODE -eq 0) "D3 fresh backend passes (exit $LASTEXITCODE)"
        Check (-not (Test-Path $marker)) "D3 a fresh backend is not rebuilt"
        Check ($out -match "up to date") "D3 reports the backend up to date"
    } finally {
        $env:OPENRGB2_BACKEND_DIR = $savedBackendDir
    }
}
catch {
    $script:Fail++
    Write-Host "  FAIL unexpected error: $_" -ForegroundColor Red
}
finally {
    if(Test-Path $Root) { Remove-Item -Recurse -Force $Root -ErrorAction SilentlyContinue }
}

Write-Host "backend_stamp_tests: $($script:Pass) passed, $($script:Fail) failed"
exit $script:Fail
