<#
.SYNOPSIS
    Backend build stamp + staleness check (audit P-7 step 6).

.DESCRIPTION
    Dot-sourced by packaging\windows\build-and-deploy.ps1 and
    scripts\build-all-and-install.ps1; defines functions only.

    The vendored backend (vendor\OpenRGB\...\OpenRGB.exe) is built by
    its own qmake recipe, outside CMake, so a changed vendor source
    used to deploy the old binary without any warning. After every
    successful recipe run the caller writes a stamp next to
    OpenRGB.exe (Write-BackendStamp); before staging,
    Test-BackendStamp compares it with the current source hash.

    The source hash covers what the recipe compiles: vendor/OpenRGB
    and src/core/playback (OpenRGB.pro adds
    ../../src/core/playback/PlaybackLoop.cpp). It is the SHA-256 of
      - git ls-files -s over those paths (the index: blob ids), and
      - for every modified, deleted or untracked, non-ignored file
        there (git status), its status and a SHA-256 of its content.
    Ignored build outputs (release\, _intermediate_*, the deploy dir)
    never count. The stamp is written after the recipe succeeds, so
    a source edited while the recipe was running is not detected.
#>

$BackendSourcePaths = @("vendor/OpenRGB", "src/core/playback")
$BackendStampName   = "openrgb2-backend.stamp"

# SHA-256 hex of the backend sources under $RepoRoot, or $null when
# $RepoRoot is not a git work tree (or git is not installed).
function Get-BackendSourceHash([string]$RepoRoot) {
    # PowerShell 5.1 turns a native command's stderr into a terminating
    # error under the callers' "Stop"; exit codes are checked instead.
    $ErrorActionPreference = "Continue"

    if(-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { return $null }

    $inside = & git.exe -C $RepoRoot rev-parse --is-inside-work-tree 2>$null
    if($LASTEXITCODE -ne 0 -or "$inside".Trim() -ne "true") { return $null }

    $index = & git.exe -C $RepoRoot ls-files -s -- @BackendSourcePaths
    if($LASTEXITCODE -ne 0) { throw "git ls-files failed in $RepoRoot" }

    # --no-optional-locks: a check must not rewrite the index.
    # -z: NUL-separated, paths unquoted.
    $status = & git.exe -C $RepoRoot --no-optional-locks status --porcelain=v1 -z `
                  --untracked-files=all --no-renames -- @BackendSourcePaths
    if($LASTEXITCODE -ne 0) { throw "git status failed in $RepoRoot" }

    $lines = New-Object System.Collections.Generic.List[string]
    foreach($l in @($index)) { if($l) { $lines.Add("index $l") } }

    $dirty = New-Object System.Collections.Generic.List[string]
    foreach($entry in ((@($status) -join "") -split "`0")) {
        if($entry.Length -lt 4) { continue }
        $code = $entry.Substring(0, 2)
        $rel  = $entry.Substring(3)
        $full = Join-Path $RepoRoot ($rel -replace '/', '\')
        $content = "missing"
        if(Test-Path -LiteralPath $full -PathType Leaf) {
            $content = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        $dirty.Add("dirty $code $rel $content")
    }
    $dirty.Sort([StringComparer]::Ordinal)
    foreach($d in $dirty) { $lines.Add($d) }

    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha   = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") }) -join "")
    } finally { $sha.Dispose() }
}

# Write the stamp for the current sources next to OpenRGB.exe in
# $BackendDir. Call it right after the recipe succeeded. Without a
# git tree no stamp is written (the check then reports "unknown").
function Write-BackendStamp([string]$RepoRoot, [string]$BackendDir) {
    $hash = Get-BackendSourceHash $RepoRoot
    if(-not $hash) { return }
    $stamp = Join-Path $BackendDir $BackendStampName
    @(
        $hash,
        "# OpenRGB2 backend source stamp: SHA-256 of vendor/OpenRGB + src/core/playback",
        "# (packaging\windows\backend-stamp.ps1), written $((Get-Date).ToString('s')) after the qmake recipe"
    ) | Set-Content -Encoding ASCII -LiteralPath $stamp
}

# Compare the stamp in $BackendDir with the current sources.
# Returns @{ State = fresh|stale|missing|unknown; Expected; Actual; Message }.
function Test-BackendStamp([string]$RepoRoot, [string]$BackendDir) {
    $actual = Get-BackendSourceHash $RepoRoot
    $stamp  = Join-Path $BackendDir $BackendStampName

    $expected = $null
    if(Test-Path -LiteralPath $stamp -PathType Leaf) {
        $first = Get-Content -LiteralPath $stamp -TotalCount 1
        if("$first".Trim() -match '^[0-9a-f]{64}$') { $expected = "$first".Trim() }
    }

    if(-not $actual) {
        return @{ State = "unknown"; Expected = $expected; Actual = $null;
                  Message = "backend staleness not checked: $RepoRoot is not a git work tree" }
    }
    if(-not $expected) {
        return @{ State = "missing"; Expected = $null; Actual = $actual;
                  Message = ("backend is stale: no build stamp ($BackendStampName) next to " +
                             "OpenRGB.exe in $BackendDir, so it cannot be matched to the current " +
                             "vendor/OpenRGB sources") }
    }
    if($expected -ne $actual) {
        return @{ State = "stale"; Expected = $expected; Actual = $actual;
                  Message = ("backend is stale: vendor/OpenRGB or src/core/playback changed since " +
                             "OpenRGB.exe in $BackendDir was built") }
    }
    return @{ State = "fresh"; Expected = $expected; Actual = $actual;
              Message = "backend is up to date with its sources (stamp $($actual.Substring(0, 12)))" }
}
