<#
.SYNOPSIS
    stage-release.ps1 derives the manifest's backend pin from
    vendor/provenance/MANIFEST.md (audit P-14). CTest:
    package_stage_pin_test (label packaging).

.DESCRIPTION
    stage-release.ps1 runs its staging at top level (no dot-source
    guard), so the pin helper is loaded from the script's own syntax
    tree -- the shipped function, not a copy -- and run against the
    repository's provenance record and against throwaway ones:

      B1  the repository's record yields the pinned fork commit and
          branch in the historical manifest format
      B2  a different recorded commit/branch is followed (no
          hard-coded string)
      B3  a record without the backend row is an error, never a
          silent default

    Exit code: number of failed checks.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$StageScript,
    [Parameter(Mandatory)][string]$RepoRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$script:Pass = 0
$script:Fail = 0
function Check([bool]$cond, [string]$name) {
    if($cond) { $script:Pass++; Write-Host "  PASS $name" }
    else      { $script:Fail++; Write-Host "  FAIL $name" -ForegroundColor Red }
}

$Root = Join-Path $env:TEMP ("openrgb2-pintest-" + [guid]::NewGuid().ToString("N").Substring(0,8))
try {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path $StageScript).Path, [ref]$tokens, [ref]$errors)
    $fn = $ast.Find({ param($n)
        $n -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $n.Name -eq "Get-BackendPin" }, $true)
    Check ($null -ne $fn) "stage-release.ps1 defines Get-BackendPin"
    if($fn) { . ([scriptblock]::Create($fn.Extent.Text)) }

    #---- B1 the repository's own record -------------------------------
    $manifestMd = Get-Content -Raw (Join-Path $RepoRoot "vendor\provenance\MANIFEST.md")
    $row = [regex]::Match($manifestMd, '(?m)^\|\s*OpenRGB submodule \(backend\).*$').Value
    $sha = [regex]::Match($row, '\b[0-9a-f]{40}\b').Value
    Check ($sha.Length -eq 40) "test reads the recorded commit ($sha)"
    if($fn) {
        $pin = Get-BackendPin $RepoRoot
        Check ($pin -eq "vendor/OpenRGB @ $sha (lianli-wireless; see vendor/provenance/MANIFEST.md)") `
              "B1 pin matches the provenance record: $pin"
    }

    #---- B2 a different record is followed ------------------------------
    $fake = Join-Path $Root "repo"
    New-Item -ItemType Directory -Force (Join-Path $fake "vendor\provenance") | Out-Null
    $other = "0123456789abcdef0123456789abcdef01234567"
    @(
        "| Component | Repository | Revision at import |",
        "| --- | --- | --- |",
        "| Parent project (plugin, tests) | ``x`` | ``ffffffffffffffffffffffffffffffffffffffff`` |",
        "| OpenRGB submodule (backend) | ``y`` | ``$other``, branch ``other-branch`` |"
    ) | Set-Content -Encoding UTF8 (Join-Path $fake "vendor\provenance\MANIFEST.md")
    if($fn) {
        $pin = Get-BackendPin $fake
        Check ($pin -eq "vendor/OpenRGB @ $other (other-branch; see vendor/provenance/MANIFEST.md)") `
              "B2 pin follows the record: $pin"
    }

    #---- B3 no backend row -> error -------------------------------------
    "| Parent project (plugin, tests) | ``x`` | ``$other`` |" |
        Set-Content -Encoding UTF8 (Join-Path $fake "vendor\provenance\MANIFEST.md")
    if($fn) {
        $threw = $false
        try { $null = Get-BackendPin $fake } catch { $threw = $true }
        Check $threw "B3 a record without the backend row is an error"
    }
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
