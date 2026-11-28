<#
.SYNOPSIS
    Task 6.2 packaging-lifecycle suite (CTest label: packaging).

.DESCRIPTION
    Offline CHECK-style harness -- drives the REAL lifecycle scripts
    (packaging/windows/*.ps1) against staged fake install trees, the
    same seam style as the engine suites driving the real EngineHost
    with injected fakes:

      T1  ownership classifier parity with ClassifyOwners rules
      T2  takeover gate: conflicts + no consent refuse; consent or
          notices-only proceed; start script carries no kill path
      T3  startup refuses SILENTLY: conflicts + no -TakeOver -> exit 3
          and nothing is spawned (no run file)
      T4  explicit -TakeOver proceeds: backend+engine spawned, run
          file written
      T5  update: new binaries in, stale files out, profiles\local +
          state preserved, .previous snapshot + journal
      T6  rollback: manifest-verified restore of previous binaries
          AND manifest.json (version), post-update user state
          preserved, corrupt backup refused
      T7  clean shutdown ordering: IPC QUIT reaches the engine while
          the backend is still up, both exit, profile file untouched;
          the UI is closed too (-Force), and reported without it
      T8  update refuses while the install is running -- engine AND
          UI (image-path verified), a same-named process with an
          UNREADABLE image path (elevated stack), and an install exe
          that is locked; rollback refuses too
      T9  backend guard: a foreign OpenRGB.exe refuses the spawn even
          under -TakeOver (the pinned backend's own kill-capable
          single-instance prompt is never triggered); an OpenRGB.exe
          at OUR pinned path mid-start is waited on, never
          double-spawned
      T10 interrupted update: a retry never retakes the snapshot (the
          only good copy) and resumes to completion
      T11 interrupted update followed by rollback restores the old
          binaries, removes what the update added, restores the
          manifest version, clears the marker
      T12 manifest paths: rooted / '..' paths refused by update and
          rollback; preserved-path match is case-insensitive
      T13 start refuses (exit 4) when OpenRGB.exe / the :6744
          listener has an unreadable image path (unverifiable --
          never mistaken for a foreign listener)
      T14 update ACL-hardening user resolution: inherited grants count,
          admin/system/well-known SIDs never do, ambiguity refuses
          (-UserSid required) instead of picking the elevating account
      T15 start: findings reach STDOUT under -File and the exit code
          matches them (3 conflict / 4 unverifiable); oversized logs
          rotate by rename only while nobody holds them

    Exit code: number of failed checks.
#>
[CmdletBinding()]
param(
    # packaging/windows source dir holding the scripts under test.
    [string]$ScriptsDir = "$PSScriptRoot\..\..\packaging\windows"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop

$script:Pass = 0
$script:Fail = 0
function Check([bool]$cond, [string]$name) {
    if($cond) { $script:Pass++; Write-Host "  PASS $name" }
    else      { $script:Fail++; Write-Host "  FAIL $name" -ForegroundColor Red }
}
function Section([string]$name) { Write-Host "== $name" -ForegroundColor Cyan }

#--------------------------------------------------------------------#
# Fixture helpers                                                     #
#--------------------------------------------------------------------#
$TestRoot = Join-Path $env:TEMP ("openrgb2-pkgtest-" + [guid]::NewGuid().ToString("N").Substring(0,8))
$Sys32    = Join-Path $env:SystemRoot "System32"
New-Item -ItemType Directory -Force $TestRoot | Out-Null

# Tracks every test-spawned process so teardown can stop OUR OWN
# procs only (never anything else).
$script:Spawned = @()
function Kill-Spawned {
    foreach($id in $script:Spawned) {
        try { Stop-Process -Id $id -Force -ErrorAction Stop } catch {}
    }
    $script:Spawned = @()
}

function Write-FakeManifest([string]$dir, [string]$version) {
    $files = @()
    Get-ChildItem $dir -Recurse -File |
        Where-Object { $_.Name -ne "manifest.json" } |
        ForEach-Object {
            $rel = ($_.FullName.Substring($dir.Length).TrimStart('\','/') -replace '\\','/')
            $files += [ordered]@{
                path   = $rel
                sha256 = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower()
                bytes  = $_.Length
                origin = "test"
            }
        }
    [ordered]@{
        package = "OpenRGB2"
        version = $version
        files   = $files
    } | ConvertTo-Json -Depth 6 |
        Set-Content -Encoding UTF8 (Join-Path $dir "manifest.json")
}

# A minimal but structurally-valid install/package tree. Exe stand-ins
# are real PE binaries so image-path verification works; $tag marks
# which "version" a tree is (inside a manifest-listed file).
function New-FakeTree([string]$dir, [string]$version, [string]$tag,
                    [string]$backendExe, [string]$engineExe) {
    foreach($d in @("backend","presets\devices","presets\effects",
                    "scripts","licenses","profiles")) {
        New-Item -ItemType Directory -Force (Join-Path $dir $d) | Out-Null
    }
    Copy-Item $engineExe  (Join-Path $dir "openrgb2-engine.exe") -Force
    Copy-Item $engineExe  (Join-Path $dir "openrgb2-ui.exe")     -Force
    Copy-Item $engineExe  (Join-Path $dir "openrgb2-ctl.exe")    -Force
    Copy-Item $backendExe (Join-Path $dir "backend\OpenRGB.exe") -Force
    Set-Content -Encoding ASCII (Join-Path $dir "build-tag.json") `
        ('{"tag":"' + $tag + '"}')
    Set-Content -Encoding ASCII (Join-Path $dir "presets\devices\d.json") "{}"
    Set-Content -Encoding ASCII (Join-Path $dir "presets\effects\e.json") "{}"
    Set-Content -Encoding ASCII (Join-Path $dir "profiles\daniel-pc.json") "{}"
    Set-Content -Encoding ASCII (Join-Path $dir "NOTICES.txt") "test notices"
    Write-FakeManifest $dir $version
}

function Read-Tag([string]$dir) {
    return ((Get-Content (Join-Path $dir "build-tag.json") -Raw |
             ConvertFrom-Json).tag)
}
function Sha([string]$path) {
    return (Get-FileHash -Algorithm SHA256 $path).Hash.ToLower()
}

# Dot-source the scripts under test (they guard on InvocationName).
. (Join-Path $ScriptsDir "start-openrgb2.ps1")
. (Join-Path $ScriptsDir "stop-openrgb2.ps1")
. (Join-Path $ScriptsDir "update-openrgb2.ps1")
. (Join-Path $ScriptsDir "rollback-openrgb2.ps1")

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$startSrc = Get-Content (Join-Path $ScriptsDir "start-openrgb2.ps1") -Raw

# Update/rollback sections that are not ABOUT the running check pass an
# empty process table: a real (elevated) OpenRGB2 on the host would
# otherwise make them refuse as unverifiable.
$NoProcs = @()

# The host may run a real OpenRGB2 stack -- elevated, so its image
# paths are unreadable here and every same-named lookup would see an
# UNVERIFIABLE process (audit P-6: the suite failed whenever one ran).
# Every check that drives the real scripts' same-named discovery
# therefore injects a process table holding ONLY this suite's own
# stand-ins (live Get-Process objects, so image paths are real); the
# unverifiable case is covered with an explicit synthetic entry.
function Get-SpawnedTable {
    return @(foreach($id in $script:Spawned) {
        Get-Process -Id $id -ErrorAction SilentlyContinue })
}

# Body runs under try/finally: an exception mid-run must not leak the
# stand-in processes or the temp tree (it counts as a failure).
try {

#====================================================================#
Section "T1 classifier parity (synthetic process list)"
#====================================================================#
$procs = @(
    [pscustomobject]@{ pid=42;  exe="OpenRGB.exe" },        # other instance -> conflict
    [pscustomobject]@{ pid=7;   exe="openrgb.exe" },        # managed backend  -> notice
    [pscustomobject]@{ pid=50;  exe="LConnect.Service.exe" },
    [pscustomobject]@{ pid=51;  exe="FanControl.exe" },     # notice
    [pscustomobject]@{ pid=52;  exe="iCUE.exe" },
    [pscustomobject]@{ pid=53;  exe="RazerSynapse.exe" },
    [pscustomobject]@{ pid=55;  exe="logi_lamparray_service.AMD64.exe" },
    [pscustomobject]@{ pid=54;  exe="Steam.exe" },          # unrelated -- invisible
    [pscustomobject]@{ pid=999; exe="LConnect.exe" }        # self pid -- skipped
)
$f = Get-OwnerFindings -Processes $procs -SelfPid 999 -BackendPid 7
$conflicts = @($f | Where-Object conflict)
$notices   = @($f | Where-Object { -not $_.conflict })
Check ($conflicts.Count -eq 5) "conflicts: other-OpenRGB + LConnect + iCUE + Synapse + LampArray"
Check ($notices.Count -eq 2)   "notices: managed backend + FanControl"
Check (@($f | Where-Object { $_.exe -ieq "openrgb.exe" -and $_.pid -eq 7 -and -not $_.conflict }).Count -eq 1) `
      "managed backend pid exempted by listener pid"
Check (@($f | Where-Object { $_.owner -like "FanControl*" -and -not $_.conflict }).Count -eq 1) `
      "FanControl is a notice, never a conflict"
Check (@($f | Where-Object { $_.exe -ieq "Steam.exe" }).Count -eq 0) "unrelated processes not reported"

#---- mechanical parity with the C++ classifier (review fix r1) ------
# Extract the rule tokens and owner labels straight from
# src/engine/OwnershipScan.cpp and compare BOTH sets to this script's
# mirror. Parity is asserted, not commented -- a rule added or renamed
# on either side fails loudly here instead of silently diverging.
$cppSrc = Get-Content (Join-Path $RepoRoot "src\engine\OwnershipScan.cpp") -Raw
$cppBeg = $cppSrc.IndexOf("std::vector<OwnerFinding> ClassifyOwners")
$cppEnd = $cppSrc.IndexOf("IsLegacySceneProducerName")
Check ($cppBeg -ge 0 -and $cppEnd -gt $cppBeg) "OwnershipScan.cpp classifier region located"
$cppBody = $cppSrc.Substring($cppBeg, $cppEnd - $cppBeg)
$cppTokens = @(
    ([regex]::Matches($cppBody, 'Contains\(\s*(?:compact|name)\s*,\s*"([^"]+)"\s*\)') +
     [regex]::Matches($cppBody, 'name\s*==\s*"([^"]+)"')) |
        ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } |
        Sort-Object -Unique)
$cppOwners = @([regex]::Matches($cppBody, 'owner\s*=\s*"([^"]+)"') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)

$psBeg  = $startSrc.IndexOf("function Get-OwnerFindings")
$psEnd  = $startSrc.IndexOf("function Find-ListenerPid")
$psBody = $startSrc.Substring($psBeg, $psEnd - $psBeg)
$psTokens = @(
    ([regex]::Matches($psBody, '\.Contains\("([^"]+)"\)') +
     [regex]::Matches($psBody, '-eq\s+"([^"]+)"')) |
        ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } |
        Sort-Object -Unique)
$psOwners = @([regex]::Matches($psBody, 'owner\s*=\s*"([^"]+)"') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)

$tokDiff = @(Compare-Object $cppTokens $psTokens)
Check ($cppTokens.Count -gt 0 -and $tokDiff.Count -eq 0) `
      "rule-token parity with OwnershipScan.cpp ($($cppTokens.Count) tokens)"
foreach($d in $tokDiff) {
    Write-Host "    drift: $($d.SideIndicator) $($d.InputObject)"
}
# R-10: the stable machine key rides with every finding on both
# sides -- the UI picks guidance by key, so a drift would show the
# wrong steps.
$cppKeys = @([regex]::Matches($cppBody, 'key\s*=\s*"([^"]+)"') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
$psKeys  = @([regex]::Matches($psBody, 'key\s*=\s*"([^"]+)"') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
$keyDiff = @(Compare-Object $cppKeys $psKeys)
Check ($cppKeys.Count -gt 0 -and $keyDiff.Count -eq 0) `
      "owner-key parity with OwnershipScan.cpp ($($cppKeys.Count) keys)"
foreach($d in $keyDiff) {
    Write-Host "    drift: $($d.SideIndicator) $($d.InputObject)"
}
Check (@($f | Where-Object { [string]::IsNullOrEmpty($_.key) }).Count -eq 0) `
      "every script finding carries a key"
Check (@($f | Where-Object { $_.exe -ieq "logi_lamparray_service.AMD64.exe" -and $_.key -eq "lamparray" }).Count -eq 1) `
      "LampArray finding keyed lamparray"
# R-10: the refusal output carries each finding's key, so the UI's
# takeover dialog can add the matching hint without re-classifying.
$findingText = Write-Findings -Findings $f 6>&1 | Out-String -Width 4096
Check ($findingText -match 'ownership CONFLICT: LampArray service .*\[key: lamparray\]') `
      "CONFLICT lines end with the finding key"
$ownDiff = @(Compare-Object $cppOwners $psOwners)
Check ($ownDiff.Count -eq 0) "owner-label parity with OwnershipScan.cpp"
foreach($d in $ownDiff) {
    Write-Host "    drift: $($d.SideIndicator) $($d.InputObject)"
}

#====================================================================#
Section "T2 takeover gate + no kill path"
#====================================================================#
Check (-not (Assert-TakeoverConsent -Findings $f -Consent $false)) `
      "conflicts + no consent -> startup refused"
Check (Assert-TakeoverConsent -Findings $f -Consent $true) `
      "conflicts + explicit consent -> proceed"
Check (Assert-TakeoverConsent -Findings $notices -Consent $false) `
      "notices only -> proceed without consent"
Check ($startSrc -notmatch 'Stop-Process\s+-|taskkill|Stop-Computer') `
      "start script contains no process-termination call"
$stopSrc = Get-Content (Join-Path $ScriptsDir "stop-openrgb2.ps1") -Raw
Check ([regex]::Matches($stopSrc, 'Stop-Process\s+-').Count -eq 3 -and
       [regex]::Matches($stopSrc, '\$Force -and \(Test-OwnProcess').Count -eq 3) `
      "stop script Stop-Process only behind -Force + own-process verification (UI, engine, backend)"

#====================================================================#
Section "T3 silent startup refused (conflicts, no -TakeOver)"
#====================================================================#
$instA = Join-Path $TestRoot "install-a"
# cmd.exe stand-ins stay alive regardless of the args the launcher
# passes (interactive console) — they never bind :6744, exercising
# the real spawn + wait + warn path.
New-FakeTree $instA "1.0.0" "A" (Join-Path $Sys32 "cmd.exe") (Join-Path $Sys32 "cmd.exe")

$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $false -ScanOnly $true -Elevated $false `
    -BackendWaitSeconds 1 -ProcessList $procs -ListenerPidOverride 0
Check ($rc -eq 3) "ScanOnly with conflicts exits 3"
$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $false -ScanOnly $true -Elevated $false `
    -BackendWaitSeconds 1 -ListenerPidOverride 0 `
    -ProcessList @([pscustomobject]@{pid=54;exe="Steam.exe"})
Check ($rc -eq 0) "ScanOnly clean exits 0"

$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $false -ScanOnly $false -Elevated $false `
    -BackendWaitSeconds 1 -ProcessList $procs -ListenerPidOverride 0
Check ($rc -eq 3) "conflicts without -TakeOver -> refused (3)"
Check (-not (Test-Path (Join-Path $instA "state\openrgb2.run.json"))) `
      "refused startup spawns nothing (no run file)"

#====================================================================#
Section "T4 explicit -TakeOver proceeds"
#====================================================================#
# cmd exes stay alive regardless of arguments; the backend never
# binds :6744 (warning path), the engine proc survives the startup
# check, the run file records both. The conflict list deliberately
# carries NO openrgb.exe: a foreign one now refuses the backend spawn
# (T9) and a fake pid has no resolvable image path.
$procsTakeover = @(
    [pscustomobject]@{ pid=50;  exe="LConnect.Service.exe" },
    [pscustomobject]@{ pid=51;  exe="FanControl.exe" },
    [pscustomobject]@{ pid=52;  exe="iCUE.exe" },
    [pscustomobject]@{ pid=53;  exe="RazerSynapse.exe" },
    [pscustomobject]@{ pid=54;  exe="Steam.exe" }
)
$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $true -ScanOnly $false -Elevated $false `
    -BackendWaitSeconds 1 -ProcessList $procsTakeover -ListenerPidOverride 0
Check ($rc -eq 0) "conflicts + -TakeOver -> started (0)"
$runFile = Join-Path $instA "state\openrgb2.run.json"
Check (Test-Path $runFile) "run file written"
if(Test-Path $runFile) {
    $run = Get-Content $runFile -Raw | ConvertFrom-Json
    Check ($run.takeover -eq $true) "run file records takeover=true"
    $script:Spawned += @([int]$run.engine_pid, [int]$run.backend_pid)
    Check (@($script:Spawned | Where-Object {
        $id = $_; try { $null -ne (Get-Process -Id $id -ErrorAction Stop) } catch { $false }
    }).Count -ge 1) "spawned processes recorded and alive"
}
Kill-Spawned
Remove-Item -Force -ErrorAction SilentlyContinue $runFile

#====================================================================#
Section "T4b engine dies at startup: the backend this start spawned is not orphaned"
#====================================================================#
# The engine stand-in (whoami.exe) rejects the engine's arguments and
# exits at once; the backend stand-in (cmd.exe) stays up and has no
# shutdown event (an older backend), so it cannot be closed
# gracefully from here. It used to be left running with no run file
# (audit P-14); now it is recorded so stop-openrgb2.ps1 closes it.
$instI = Join-Path $TestRoot "install-i"
New-FakeTree $instI "1.0.0" "I" (Join-Path $Sys32 "cmd.exe") (Join-Path $Sys32 "whoami.exe")
$runFileI = Join-Path $instI "state\openrgb2.run.json"
$rc = Invoke-ManagedStart -InstallDir $instI -Scene "" -NoUI $true `
    -TakeOver $false -ScanOnly $false -Elevated $false `
    -BackendWaitSeconds 1 -ListenerPidOverride 0 `
    -ProcessList @([pscustomobject]@{pid=54; exe="Steam.exe"})
Check ($rc -eq 1) "engine that exits during startup -> 1"
$orphanPid = 0
if(Test-Path $runFileI) {
    $runI = Get-Content $runFileI -Raw | ConvertFrom-Json
    $orphanPid = [int]$runI.backend_pid
    $script:Spawned += $orphanPid
    Check ([int]$runI.engine_pid -eq 0) "run file records no engine"
} else { Check $false "run file records the backend this start spawned" }
# stop-openrgb2.ps1 signals a run-file pid only after this same
# image-path verification (Resolve-OwnPid -> Test-OwnProcess).
Check ($orphanPid -gt 0 -and
       (Test-OwnProcess $orphanPid (Join-Path $instI "backend\OpenRGB.exe"))) `
      "the recorded pid is the still-running backend this start spawned (stop can verify it)"
# whatever happened above, never leak the stand-in (matched by OUR
# temp image path only)
$script:Spawned += @(Get-Process -Name "OpenRGB" -ErrorAction SilentlyContinue |
    Where-Object { try { $_.Path -eq (Join-Path $instI "backend\OpenRGB.exe") } catch { $false } } |
    ForEach-Object { $_.Id })
Kill-Spawned

#====================================================================#
Section "T5 update preserves user state"
#====================================================================#
$instB = Join-Path $TestRoot "install-b"
New-FakeTree $instB "1.0.0" "V1" (Join-Path $Sys32 "notepad.exe") (Join-Path $Sys32 "notepad.exe")
Set-Content -Encoding ASCII (Join-Path $instB "obsolete.dll") "old-binary"
# re-write manifest including obsolete.dll, THEN seed user state
Write-FakeManifest $instB "1.0.0"
New-Item -ItemType Directory -Force (Join-Path $instB "profiles\local") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $instB "state\backend") | Out-Null
Set-Content -Encoding ASCII (Join-Path $instB "profiles\local\engine-state.json") '{"schema_version":3,"scenes":[]}'
Set-Content -Encoding ASCII (Join-Path $instB "state\backend\OpenRGB.json") '{"devices":{}}'
$profileHash = Sha (Join-Path $instB "profiles\local\engine-state.json")
$cfgHash     = Sha (Join-Path $instB "state\backend\OpenRGB.json")

$pkgV2 = Join-Path $TestRoot "pkg-v2"
New-FakeTree $pkgV2 "2.0.0" "V2" (Join-Path $Sys32 "notepad.exe") (Join-Path $Sys32 "notepad.exe")
Set-Content -Encoding ASCII (Join-Path $pkgV2 "new-dependency.dll") "new-binary"
Write-FakeManifest $pkgV2 "2.0.0"

$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instB -SkipVerify $false `
    -SkipAclHardening $true -ProcessList $NoProcs
Check ($rc -eq 0) "update returns 0"
Check ((Read-Manifest $instB).version -eq "2.0.0") "install manifest reports 2.0.0"
Check (-not (Test-Path (Join-Path $instB ".update-in-progress.json"))) "update marker cleared"
Check (-not (Test-Path "$instB.previous.tmp")) "no snapshot temp dir left behind"
Check ((Read-Tag $instB) -eq "V2") "new binaries applied"
Check (Test-Path (Join-Path $instB "new-dependency.dll")) "update-added file present"
Check (-not (Test-Path (Join-Path $instB "obsolete.dll"))) "manifest-diff removed stale file"
Check ((Sha (Join-Path $instB "profiles\local\engine-state.json")) -eq $profileHash) `
      "profiles\local\engine-state.json preserved"
Check ((Sha (Join-Path $instB "state\backend\OpenRGB.json")) -eq $cfgHash) `
      "state\backend\OpenRGB.json preserved"
Check ((Read-Tag "$instB.previous") -eq "V1") ".previous snapshot holds old version"
$journalRead = Get-Content (Join-Path $instB "state\updates.json") -Raw | ConvertFrom-Json
$journal = @($journalRead)
$lastEntry = $journal[$journal.Count - 1]
Check ($lastEntry.action -eq "update" -and
       $lastEntry.to_version -eq "2.0.0") "update journaled"

#====================================================================#
Section "T6 manifest-verified rollback, user state never rolled back"
#====================================================================#
# state created AFTER the update must survive the rollback.
Set-Content -Encoding ASCII (Join-Path $instB "profiles\local\session-2.json") '{"new":true}'

# corrupt backup refuses BEFORE touching the install
Set-Content -Encoding ASCII "$instB.previous\obsolete.dll" "TAMPERED"
$rc = Invoke-PackageRollback -InstallDir $instB -BackupDir "" -SkipVerify $false `
    -ProcessList $NoProcs
Check ($rc -eq 1) "corrupt backup refused"
Check ((Read-Tag $instB) -eq "V2") "refused rollback changed nothing"
Set-Content -Encoding ASCII "$instB.previous\obsolete.dll" "old-binary"

$rc = Invoke-PackageRollback -InstallDir $instB -BackupDir "" -SkipVerify $false `
    -ProcessList $NoProcs
Check ($rc -eq 0) "rollback returns 0"
Check ((Read-Tag $instB) -eq "V1") "previous binaries restored"
Check ((Read-Manifest $instB).version -eq "1.0.0") `
      "install manifest.json restored (reports 1.0.0 after rollback)"
Check (Test-Path (Join-Path $instB "obsolete.dll")) "old file restored"
Check (-not (Test-Path (Join-Path $instB "new-dependency.dll"))) "update-added file removed"
Check (Test-Path (Join-Path $instB "profiles\local\session-2.json")) `
      "post-update user state survived rollback"
Check ((Sha (Join-Path $instB "profiles\local\engine-state.json")) -eq $profileHash) `
      "durable profile byte-identical through update+rollback"
$journalRead = Get-Content (Join-Path $instB "state\updates.json") -Raw | ConvertFrom-Json
$journal = @($journalRead)
$lastEntry = $journal[$journal.Count - 1]
Check ($lastEntry.action -eq "rollback") "rollback journaled"

#====================================================================#
Section "T7 shutdown ordering: QUIT -> engine exits -> backend closes"
#====================================================================#
$instC = Join-Path $TestRoot "install-c"
New-FakeTree $instC "1.0.0" "C" `
    (Join-Path $Sys32 "WindowsPowerShell\v1.0\powershell.exe") `
    (Join-Path $Sys32 "WindowsPowerShell\v1.0\powershell.exe")
New-Item -ItemType Directory -Force (Join-Path $instC "profiles\local") | Out-Null
Set-Content -Encoding ASCII (Join-Path $instC "profiles\local\engine-state.json") '{"schema_version":3}'
$profBefore = Sha (Join-Path $instC "profiles\local\engine-state.json")

# fake engine + backend: long sleeps under OUR image paths
$eng = Start-Process -FilePath (Join-Path $instC "openrgb2-engine.exe") -PassThru `
       -WindowStyle Hidden -ArgumentList "-NoProfile","-Command","Start-Sleep 300"
$bk  = Start-Process -FilePath (Join-Path $instC "backend\OpenRGB.exe") -PassThru `
       -WindowStyle Hidden -ArgumentList "-NoProfile","-Command","Start-Sleep 300"
$script:Spawned += @($eng.Id, $bk.Id)

# ctl stub: records backend liveness AT QUIT TIME, then simulates the
# engine's own exit (the ordered teardown a real engine runs on QUIT).
$marker  = Join-Path $TestRoot "ctl-marker.json"
$ctlStub = Join-Path $TestRoot "fake-ctl.ps1"
# (the `$env: refs stay escaped — the stub reads them at ITS runtime)
@"
`$alive = `$false
try { `$alive = (`$null -ne (Get-Process -Id `$env:OPENRGB2_TEST_BACKEND_PID -ErrorAction Stop)) } catch {}
[ordered]@{ verb = `$args[0]; backend_alive_at_quit = `$alive } |
    ConvertTo-Json | Set-Content -Encoding UTF8 "$($marker -replace '\\','\\')"
try { Stop-Process -Id `$env:OPENRGB2_TEST_ENGINE_PID -Force -ErrorAction Stop } catch {}
"@ | Set-Content -Encoding UTF8 $ctlStub

$env:OPENRGB2_TEST_ENGINE_PID  = "$($eng.Id)"
$env:OPENRGB2_TEST_BACKEND_PID = "$($bk.Id)"

New-Item -ItemType Directory -Force (Join-Path $instC "state") | Out-Null
[ordered]@{
    install_dir = $instC; engine_pid = $eng.Id; backend_pid = $bk.Id
    engine_exe  = Join-Path $instC "openrgb2-engine.exe"
    backend_exe = Join-Path $instC "backend\OpenRGB.exe"
} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $instC "state\openrgb2.run.json")

$rc = Invoke-ManagedStop -InstallDir $instC -TimeoutSeconds 3 `
    -Force $true -CtlExe $ctlStub -ProcessList (Get-SpawnedTable)
Check ($rc -eq 0) "clean stop returns 0"
if(Test-Path $marker) {
    $m = Get-Content $marker -Raw | ConvertFrom-Json
    Check ($m.verb -eq "quit") "IPC QUIT verb sent to engine"
    Check ($m.backend_alive_at_quit -eq $true) `
          "QUIT reached engine BEFORE backend close (ordering)"
} else { Check $false "ctl stub ran (marker written)" }
$engAlive = $false; $bkAlive = $false
try { $engAlive = $null -ne (Get-Process -Id $eng.Id -ErrorAction Stop) } catch {}
try { $bkAlive  = $null -ne (Get-Process -Id $bk.Id  -ErrorAction Stop) } catch {}
Check (-not $engAlive) "engine exited"
Check (-not $bkAlive)  "backend exited"
Check ((Sha (Join-Path $instC "profiles\local\engine-state.json")) -eq $profBefore) `
      "profile byte-identical through shutdown"
Check (-not (Test-Path (Join-Path $instC "state\openrgb2.run.json"))) `
      "run file cleared"

# nothing-running stop is a clean no-op.
$rc = Invoke-ManagedStop -InstallDir $instC -TimeoutSeconds 1 `
    -Force $false -CtlExe $ctlStub -ProcessList (Get-SpawnedTable)
Check ($rc -eq 0) "stop with nothing running exits 0"
# Same-named processes whose image path cannot be read (an elevated
# stack seen from this shell) are reported honestly -- exit 1, never
# a silent 0 -- and never touched.
foreach($name in @("openrgb2-ui", "openrgb2-engine", "OpenRGB")) {
    $hiddenProc = @([pscustomobject]@{ ProcessName=$name; Id=4242; Path=$null })
    $rc = Invoke-ManagedStop -InstallDir $instC -TimeoutSeconds 1 `
        -Force $true -CtlExe $ctlStub -ProcessList $hiddenProc
    Check ($rc -eq 1) "stop exits 1 when a same-named $name is unverifiable"
}

# UI step: closing the window hides the UI to the tray, so without
# -Force the still-running UI is reported (exit 1) and left alone;
# with -Force OUR UI (image path verified) is stopped.
$uiProc = Start-Process -FilePath (Join-Path $instC "openrgb2-ui.exe") -PassThru `
          -WindowStyle Hidden -ArgumentList "-NoProfile","-Command","Start-Sleep 300"
$script:Spawned += $uiProc.Id
Start-Sleep -Milliseconds 400
$rc = Invoke-ManagedStop -InstallDir $instC -TimeoutSeconds 1 `
    -Force $false -CtlExe $ctlStub -ProcessList (Get-SpawnedTable)
$uiAlive = $false
try { $uiAlive = $null -ne (Get-Process -Id $uiProc.Id -ErrorAction Stop) } catch {}
Check ($rc -eq 1 -and $uiAlive) "UI still running without -Force -> reported (exit 1), not killed"
$rc = Invoke-ManagedStop -InstallDir $instC -TimeoutSeconds 1 `
    -Force $true -CtlExe $ctlStub -ProcessList (Get-SpawnedTable)
$uiAlive = $false
try { $uiAlive = $null -ne (Get-Process -Id $uiProc.Id -ErrorAction Stop) } catch {}
Check (-not $uiAlive) "stop -Force closes OUR UI (verified by image path)"
Check ($rc -eq 0) "stop -Force with the UI exits 0"
Kill-Spawned

#====================================================================#
Section "T8 update refuses while install is running"
#====================================================================#
$instD = Join-Path $TestRoot "install-d"
New-FakeTree $instD "1.0.0" "D" (Join-Path $Sys32 "cmd.exe") (Join-Path $Sys32 "cmd.exe")
$running = Start-Process -FilePath (Join-Path $instD "openrgb2-engine.exe") -PassThru
$script:Spawned += $running.Id
Start-Sleep -Milliseconds 500
$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instD -SkipVerify $true `
    -SkipAclHardening $true -ProcessList (Get-SpawnedTable)
Check ($rc -eq 1) "update refused while engine running"
Check ((Read-Tag $instD) -eq "D") "refused update changed nothing"
Check (-not (Test-Path "$instD.previous")) "refused update took no snapshot"
Kill-Spawned

# a running UI locks openrgb2-ui.exe -> update must refuse just the
# same (review fix r1: the check used to watch engine+backend only).
$ui = Start-Process -FilePath (Join-Path $instD "openrgb2-ui.exe") -PassThru
$script:Spawned += $ui.Id
Start-Sleep -Milliseconds 500
$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instD -SkipVerify $true `
    -SkipAclHardening $true -ProcessList (Get-SpawnedTable)
Check ($rc -eq 1) "update refused while UI running"
Check ((Read-Tag $instD) -eq "D") "UI-blocked update changed nothing"
# rollback refuses while running too (needs a valid backup present
# first -- the running check must fire before any restore).
New-FakeTree "$instD.previous" "0.9.0" "D0" `
    (Join-Path $Sys32 "cmd.exe") (Join-Path $Sys32 "cmd.exe")
$rc = Invoke-PackageRollback -InstallDir $instD -BackupDir "" -SkipVerify $true `
    -ProcessList (Get-SpawnedTable)
Check ($rc -eq 1) "rollback refused while UI running"
Check ((Read-Tag $instD) -eq "D") "refused rollback changed nothing"
Kill-Spawned
Start-Sleep -Milliseconds 300

# A same-named process whose image path is unreadable (an elevated
# stack seen from a non-elevated shell) is unverifiable -> refuse.
$hidden = @([pscustomobject]@{ ProcessName="openrgb2-engine"; Id=4242; Path=$null })
$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instD -SkipVerify $true `
    -SkipAclHardening $true -ProcessList $hidden
Check ($rc -eq 1) "update refused while a same-named process is unverifiable"
Check ((Read-Tag $instD) -eq "D" -and -not (Test-Path "$instD.previous.tmp")) `
      "unverifiable-refused update changed nothing"
$rc = Invoke-PackageRollback -InstallDir $instD -BackupDir "" -SkipVerify $true `
    -ProcessList $hidden
Check ($rc -eq 1) "rollback refused while a same-named process is unverifiable"
# a foreign same-named process (readable, other path) does not block
$foreignTable = @([pscustomobject]@{ ProcessName="OpenRGB"; Id=4243
                                     Path="C:\Program Files\OpenRGB\OpenRGB.exe" })
Check ($null -eq (Test-InstallRunning $instD -ProcessList $foreignTable)) `
      "a foreign same-named process with a readable path does not block"

# An install exe held open (a loaded image anywhere) -> refuse.
$lock = [IO.File]::Open((Join-Path $instD "openrgb2-ctl.exe"), [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instD -SkipVerify $true `
        -SkipAclHardening $true -ProcessList $NoProcs
    Check ($rc -eq 1) "update refused while an install exe is locked"
    Check ((Read-Tag $instD) -eq "D") "lock-refused update changed nothing"
} finally { $lock.Dispose() }

#====================================================================#
Section "T8b stop: windowless backend fallback; backend waits for the engine"
#====================================================================#
# A headless backend owns NO window, so the taskkill (no /F) fallback
# writes an error to stderr -- under Windows PowerShell 5.1 with
# $ErrorActionPreference = "Stop" that "2>&1" line used to become a
# terminating error and abort the whole stop (audit B-2).
# The backend stand-in is a copy of PING.EXE: unlike a powershell.exe
# stand-in (.NET keeps a hidden top-level window, so taskkill succeeds)
# it owns no window at all, exactly like the headless --server backend.
$instG = Join-Path $TestRoot "install-g"
$psExe = Join-Path $Sys32 "WindowsPowerShell\v1.0\powershell.exe"
New-FakeTree $instG "1.0.0" "G" (Join-Path $Sys32 "PING.EXE") $psExe
New-Item -ItemType Directory -Force (Join-Path $instG "state") | Out-Null
function Start-Windowless([string]$exe) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = if($exe -like "*OpenRGB.exe") { "-n 300 127.0.0.1" }
                     else { '-NoProfile -Command "Start-Sleep 300"' }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true          # no console window at all
    $psi.RedirectStandardOutput = $true  # keep ping's lines out of the log
    $p = [Diagnostics.Process]::Start($psi)
    $script:Spawned += $p.Id
    return $p
}
function Write-RunFile([string]$dir, [int]$enginePid, [int]$backendPid) {
    [ordered]@{
        install_dir = $dir; engine_pid = $enginePid; backend_pid = $backendPid
        engine_exe  = Join-Path $dir "openrgb2-engine.exe"
        backend_exe = Join-Path $dir "backend\OpenRGB.exe"
    } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $dir "state\openrgb2.run.json")
}

$engG = Start-Windowless (Join-Path $instG "openrgb2-engine.exe")
$bkG  = Start-Windowless (Join-Path $instG "backend\OpenRGB.exe")
Start-Sleep -Milliseconds 400
$env:OPENRGB2_TEST_ENGINE_PID  = "$($engG.Id)"
$env:OPENRGB2_TEST_BACKEND_PID = "$($bkG.Id)"
Write-RunFile $instG $engG.Id $bkG.Id
$threw = $null
try {
    $rc = Invoke-ManagedStop -InstallDir $instG -TimeoutSeconds 2 `
        -Force $true -CtlExe $ctlStub
} catch { $threw = $_.Exception.Message }
Check ($null -eq $threw) "windowless backend: the taskkill fallback never aborts the stop ($threw)"
$bkAlive = $false
try { $bkAlive = $null -ne (Get-Process -Id $bkG.Id -ErrorAction Stop) } catch {}
Check (-not $bkAlive) "windowless backend still reached the -Force step after the graceful bound"
Kill-Spawned

# The engine did NOT exit (QUIT ignored, no -Force): the backend must
# not be signaled -- closing it under a live engine breaks the
# ordering the whole script exists for. The test owns the backend's
# shutdown event, so a signal would be visible here.
$noopCtl = Join-Path $TestRoot "noop-ctl.ps1"
Set-Content -Encoding UTF8 $noopCtl "# ignores QUIT"
$engG = Start-Windowless (Join-Path $instG "openrgb2-engine.exe")
$bkG  = Start-Windowless (Join-Path $instG "backend\OpenRGB.exe")
Start-Sleep -Milliseconds 400
$evG = New-Object Threading.EventWaitHandle($false,
           [Threading.EventResetMode]::ManualReset,
           "Local\OpenRGB2-Backend-Shutdown-$($bkG.Id)")
try {
    Write-RunFile $instG $engG.Id $bkG.Id
    $rc = Invoke-ManagedStop -InstallDir $instG -TimeoutSeconds 1 `
        -Force $false -CtlExe $noopCtl
    Check ($rc -eq 1) "engine outlived the bound -> stop exits 1"
    Check (-not $evG.WaitOne(0)) "backend NOT signaled while the engine still runs"
    $bkAlive = $false
    try { $bkAlive = $null -ne (Get-Process -Id $bkG.Id -ErrorAction Stop) } catch {}
    Check $bkAlive "backend left running for the retry"
    Check (Test-Path (Join-Path $instG "state\openrgb2.run.json")) "run file kept for the retry"
} finally { $evG.Dispose() }
Kill-Spawned

#====================================================================#
Section "T9 backend guard: foreign OpenRGB.exe refuses the spawn"
#====================================================================#
# The pinned backend's own single-instance dialog can TerminateProcess
# a foreign OpenRGB.exe -- the launcher must REFUSE the spawn rather
# than trigger that prompt (the vendored cli offers no skip flag).
# Real processes make the image-path check meaningful: a foreign copy
# at a foreign path refuses; a copy at OUR pinned path is the
# still-starting managed backend -- a notice, waited on, never
# double-spawned.
$foreignDir = Join-Path $TestRoot "foreign-bin"
New-Item -ItemType Directory -Force $foreignDir | Out-Null
Copy-Item (Join-Path $Sys32 "cmd.exe") (Join-Path $foreignDir "OpenRGB.exe") -Force
$foreign = Start-Process -FilePath (Join-Path $foreignDir "OpenRGB.exe") -PassThru
$script:Spawned += $foreign.Id
Start-Sleep -Milliseconds 400

$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $true -ScanOnly $false -Elevated $false `
    -BackendWaitSeconds 1 -ListenerPidOverride 0 `
    -ProcessList @([pscustomobject]@{pid=$foreign.Id; exe="OpenRGB.exe"},
                  [pscustomobject]@{pid=54; exe="Steam.exe"})
Check ($rc -eq 3) "foreign OpenRGB.exe + -TakeOver -> startup refused (3)"
$runFileA = Join-Path $instA "state\openrgb2.run.json"
Check (-not (Test-Path $runFileA)) "refused startup spawns nothing"
$foreignAlive = $false
try { $foreignAlive = $null -ne (Get-Process -Id $foreign.Id -ErrorAction Stop) } catch {}
Check $foreignAlive "foreign OpenRGB.exe left running -- never touched"
Kill-Spawned

# own-path OpenRGB.exe mid-start: reclassified as OUR backend (a
# notice, not a conflict -- no consent needed), waited on, never
# double-spawned.
$ownBk = Start-Process -FilePath (Join-Path $instA "backend\OpenRGB.exe") -PassThru
$script:Spawned += $ownBk.Id
Start-Sleep -Milliseconds 400
$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $false -ScanOnly $false -Elevated $false `
    -BackendWaitSeconds 1 -ListenerPidOverride 0 `
    -ProcessList @([pscustomobject]@{pid=$ownBk.Id; exe="OpenRGB.exe"})
Check ($rc -eq 0) "own backend mid-start -> waited + proceeded (0)"
if(Test-Path $runFileA) {
    $run = Get-Content $runFileA -Raw | ConvertFrom-Json
    Check ([int]$run.backend_pid -eq $ownBk.Id) `
          "run file records the already-running backend (no second spawn)"
    $script:Spawned += [int]$run.engine_pid
} else { Check $false "run file written after own-backend wait" }
$ownAlive = $false
try { $ownAlive = $null -ne (Get-Process -Id $ownBk.Id -ErrorAction Stop) } catch {}
Check $ownAlive "still-starting backend untouched"
Kill-Spawned
Remove-Item -Force -ErrorAction SilentlyContinue $runFileA

#====================================================================#
Section "T10 interrupted update: retry keeps the snapshot, then resumes"
#====================================================================#
function New-UserStateInstall([string]$dir) {
    New-FakeTree $dir "1.0.0" "V1" (Join-Path $Sys32 "notepad.exe") (Join-Path $Sys32 "notepad.exe")
    New-Item -ItemType Directory -Force (Join-Path $dir "profiles\local") | Out-Null
    Set-Content -Encoding ASCII (Join-Path $dir "profiles\local\engine-state.json") '{"schema_version":3}'
}
$instE = Join-Path $TestRoot "install-e"
New-UserStateInstall $instE
$profE = Sha (Join-Path $instE "profiles\local\engine-state.json")
# A destination file held without write sharing makes the apply step
# fail mid-way -- the same shape as a crash or a power cut.
$blocker = Join-Path $instE "presets\effects\e.json"
$lock = [IO.File]::Open($blocker, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instE -SkipVerify $false `
        -SkipAclHardening $true -ProcessList $NoProcs
    Check ($rc -eq 1) "interrupted update returns 1"
    Check (Test-Path (Join-Path $instE ".update-in-progress.json")) "interrupted update leaves its marker"
    Check ((Read-Tag "$instE.previous") -eq "V1") "snapshot holds the pre-update version"
    # retry while still failing: the snapshot must NOT be retaken from
    # the half-updated install (the old code deleted the good one)
    $rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instE -SkipVerify $false `
        -SkipAclHardening $true -ProcessList $NoProcs
    Check ($rc -eq 1) "retry that fails again returns 1"
    Check ((Read-Tag "$instE.previous") -eq "V1") "failed retry kept the good snapshot (V1)"
    Check ((Read-Manifest "$instE.previous").version -eq "1.0.0") "snapshot manifest still 1.0.0"
} finally { $lock.Dispose() }
$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instE -SkipVerify $false `
    -SkipAclHardening $true -ProcessList $NoProcs
Check ($rc -eq 0) "retry after the cause is gone resumes and completes"
Check ((Read-Tag $instE) -eq "V2" -and (Read-Manifest $instE).version -eq "2.0.0") "resumed update installed 2.0.0"
Check ((Read-Tag "$instE.previous") -eq "V1") "snapshot still the pre-update version after resume"
Check (-not (Test-Path (Join-Path $instE ".update-in-progress.json"))) "marker cleared after resume"
$journal = @(Get-Content (Join-Path $instE "state\updates.json") -Raw | ConvertFrom-Json)
$lastEntry = $journal[$journal.Count - 1]
Check ($lastEntry.from_version -eq "1.0.0" -and $lastEntry.to_version -eq "2.0.0") `
      "resumed update journaled 1.0.0 -> 2.0.0"
Check ((Sha (Join-Path $instE "profiles\local\engine-state.json")) -eq $profE) "user state preserved through interruption"

#====================================================================#
Section "T11 interrupted update followed by rollback"
#====================================================================#
$instF = Join-Path $TestRoot "install-f"
New-UserStateInstall $instF
$profF = Sha (Join-Path $instF "profiles\local\engine-state.json")
$lock = [IO.File]::Open((Join-Path $instF "presets\effects\e.json"), [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instF -SkipVerify $false `
        -SkipAclHardening $true -ProcessList $NoProcs
} finally { $lock.Dispose() }
Check ($rc -eq 1) "update interrupted"
$rc = Invoke-PackageRollback -InstallDir $instF -BackupDir "" -SkipVerify $false `
    -ProcessList $NoProcs
Check ($rc -eq 0) "rollback over an interrupted update returns 0"
Check ((Read-Tag $instF) -eq "V1") "rollback restored V1 binaries"
Check ((Read-Manifest $instF).version -eq "1.0.0") "rollback restored the 1.0.0 manifest"
Check (-not (Test-Path (Join-Path $instF "new-dependency.dll"))) `
      "file the interrupted update added is removed (marker file list)"
Check (-not (Test-Path (Join-Path $instF ".update-in-progress.json"))) "rollback cleared the marker"
Check ((Sha (Join-Path $instF "profiles\local\engine-state.json")) -eq $profF) "user state untouched by rollback"

#====================================================================#
Section "T12 manifest path safety + case-insensitive preserved paths"
#====================================================================#
Check ((Test-PreservedPath "Profiles/Local/x.json") -and (Test-PreservedPath "STATE\backend\OpenRGB.json") -and
       -not (Test-PreservedPath "stateful.txt") -and -not (Test-PreservedPath "profiles/daniel-pc.json")) `
      "preserved-path match is case-insensitive and prefix-exact"
foreach($bad in @("../evil.dll", "..\evil.dll", "C:/Windows/evil.dll", "/abs.dll",
                  "\\server\share\x.dll", "a/../../b.dll", "x.dll:stream", "dir/./x.dll")) {
    Check (-not (Test-SafeRelPath $bad)) "unsafe manifest path rejected: $bad"
}
Check ((Test-SafeRelPath "backend/platforms/qwindows.dll") -and (Test-SafeRelPath "Qt6Core.dll")) `
      "normal manifest paths accepted"
$pkgEvil = Join-Path $TestRoot "pkg-evil"
New-FakeTree $pkgEvil "6.6.6" "EVIL" (Join-Path $Sys32 "notepad.exe") (Join-Path $Sys32 "notepad.exe")
$m = Get-Content (Join-Path $pkgEvil "manifest.json") -Raw | ConvertFrom-Json
$m.files += [pscustomobject]@{ path = "../escaped.dll"; sha256 = "00"; bytes = 0; origin = "test" }
$m | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $pkgEvil "manifest.json")
$rc = Invoke-PackageUpdate -PackageDir $pkgEvil -InstallDir $instE -SkipVerify $true `
    -SkipAclHardening $true -ProcessList $NoProcs
Check ($rc -eq 1 -and (Read-Tag $instE) -eq "V2") "update refuses a manifest with a '..' path, changes nothing"
$rc = Invoke-PackageRollback -InstallDir $instE -BackupDir $pkgEvil -SkipVerify $true `
    -ProcessList $NoProcs
Check ($rc -eq 1 -and (Read-Tag $instE) -eq "V2") "rollback refuses a backup manifest with a '..' path"

#====================================================================#
Section "T12b -InstallDir with a trailing separator"
#====================================================================#
# Resolve-Path keeps a trailing '\', and the snapshot path is built
# as "$InstallDir.previous" -- "C:\x\inst\.previous" lands INSIDE the
# install, so the update copied the install into itself (audit P-9).
$instH = Join-Path $TestRoot "install-h"
New-UserStateInstall $instH
foreach($sep in @('\', '/')) {
    $threw = $null
    try {
        $rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir ($instH + $sep) `
            -SkipVerify $false -SkipAclHardening $true -ProcessList $NoProcs
    } catch { $threw = $_.Exception.Message; $rc = -1 }
    Check ($rc -eq 0 -and $null -eq $threw) "update with -InstallDir '...$sep' returns 0 ($threw)"
    Check (-not (Test-Path (Join-Path $instH ".previous")) -and
           -not (Test-Path (Join-Path $instH ".previous.tmp"))) `
          "update with '...$sep': no snapshot inside the install"
    Check ((Test-Path "$instH.previous") -and (Read-Tag "$instH.previous") -eq "V1") `
          "update with '...$sep': snapshot is the sibling install-h.previous"
    $threw = $null
    try {
        $rc = Invoke-PackageRollback -InstallDir ($instH + $sep) -BackupDir "" `
            -SkipVerify $false -ProcessList $NoProcs
    } catch { $threw = $_.Exception.Message; $rc = -1 }
    Check ($rc -eq 0 -and (Read-Tag $instH) -eq "V1") `
          "rollback with -InstallDir '...$sep' finds the sibling snapshot ($threw)"
    Remove-Item -LiteralPath "$instH.previous" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $instH ".previous") -Recurse -Force -ErrorAction SilentlyContinue
}

#====================================================================#
Section "T13 start: unreadable image path is unverifiable, never foreign"
#====================================================================#
# PID 4 (System) is a live process whose image path no user-mode
# caller can read -- the same shape an elevated backend has from a
# non-elevated shell.
$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $true -ScanOnly $false -Elevated $false `
    -BackendWaitSeconds 1 -ListenerPidOverride 0 `
    -ProcessList @([pscustomobject]@{pid=4; exe="OpenRGB.exe"})
Check ($rc -eq 4) "OpenRGB.exe with unreadable image -> refused as unverifiable (4), even with -TakeOver"
Check (-not (Test-Path $runFileA)) "unverifiable refusal spawns nothing"
$rc = Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
    -TakeOver $true -ScanOnly $false -Elevated $false `
    -BackendWaitSeconds 1 -ListenerPidOverride 4 `
    -ProcessList @([pscustomobject]@{pid=54; exe="Steam.exe"})
Check ($rc -eq 4) ":6744 listener with unreadable image -> unverifiable (4), not a foreign listener"
Check (-not (Test-Path $runFileA)) "no engine spawned, run file untouched"

#====================================================================#
Section "T14 update: ACL-hardening user resolution never guesses"
#====================================================================#
# Injected security descriptors (no filesystem, no elevation): the
# resolver must find the installing user through INHERITED grants on a
# fresh state\, ignore admin/system/creator/well-known SIDs, and return
# nothing -- never the elevating account -- when it is not exactly one.
$u1 = "S-1-5-21-1111-2222-3333-1001"
$u2 = "S-1-5-21-1111-2222-3333-1002"
$adm500 = "S-1-5-21-1111-2222-3333-500"
function New-SddlAcl([string]$Sddl) {
    $a = New-Object Security.AccessControl.DirectorySecurity
    $a.SetSecurityDescriptorSddlForm($Sddl)
    return $a
}
function Resolve-WithAcls([string]$InstallSddl, [string]$StateSddl) {
    $map = @{ "Q:\OpenRGB2" = $InstallSddl; "Q:\OpenRGB2\state" = $StateSddl }
    $reader = { param($p) if($map[$p]) { New-SddlAcl $map[$p] } else { $null } }.GetNewClosure()
    return Resolve-InstallUserSid -InstallDir "Q:\OpenRGB2" -AclReader $reader
}
$pfLike = "O:BAG:SYD:AI(A;OICIID;FA;;;SY)(A;OICIID;FA;;;BA)(A;OICIID;0x1200a9;;;BU)(A;OICIIOID;GA;;;CO)(A;OICIID;0x1200a9;;;S-1-15-2-1)"
Check ((Resolve-WithAcls "O:${u1}G:SYD:AI(A;OICIID;FA;;;$u1)" $null) -eq $u1) `
    "install owned by the user -> that user"
Check ((Resolve-WithAcls "O:BAG:SYD:AI(A;OICIID;FA;;;SY)(A;OICIID;FA;;;BA)" `
        "O:BAG:SYD:AI(A;OICIID;FA;;;SY)(A;OICIID;FA;;;BA)(A;OICIID;FA;;;$u1)") -eq $u1) `
    "Administrators-owned install, fresh state\ with only INHERITED grants -> the user"
Check ((Resolve-WithAcls "O:BAG:SYD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;BU)" `
        "O:BAG:SYD:AI(A;OICI;0x1301bf;;;$u1)(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)(A;OICIID;0x1200a9;;;BU)") -eq $u1) `
    "hardened install: explicit Modify on state\ -> the user"
Check ((Resolve-WithAcls "O:BAG:SYD:AI(A;OICIID;FA;;;BA)(A;OICIID;FA;;;$adm500)(A;OICIID;0x1301bf;;;$u1)" `
        "O:BAG:SYD:AI(A;OICIID;FA;;;BA)(A;OICIID;FA;;;$adm500)(A;OICIID;0x1301bf;;;$u1)") -eq $u1) `
    "built-in Administrator account (RID 500) is never the installing user"
Check ($null -eq (Resolve-WithAcls $pfLike $null)) `
    "Program Files-like folder, no state\: admin/system/creator/app-package SIDs only -> unresolved"
Check ($null -eq (Resolve-WithAcls $pfLike "O:BAG:SYD:AI(A;OICI;0x1301bf;;;$u1)(A;OICI;0x1301bf;;;$u2)")) `
    "two users granted write on state\ -> ambiguous, unresolved"
Check ($null -eq (Resolve-WithAcls $pfLike "O:BAG:SYD:AI(A;OICIIO;FA;;;$u1)")) `
    "inherit-only grant is not access to state\ itself -> unresolved"
Check ($null -eq (Resolve-WithAcls $pfLike "O:BAG:SYD:AI(A;OICI;0x1200a9;;;$u1)")) `
    "read-only grant does not make an owner of user state -> unresolved"
$me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$r = & { function Resolve-InstallUserSid { return $null }
         Resolve-AclUserSid "Q:\OpenRGB2" "" }
Check (($null -eq $r.Sid) -and $r.Error -match '-UserSid' -and $r.Error -match 'nothing was changed') `
    "unresolved -> hardening refused with a -UserSid instruction (not the elevating account $me)"
$r = Resolve-AclUserSid "Q:\OpenRGB2" "not-a-sid"
Check (($null -eq $r.Sid) -and $r.Error -match 'not a valid SID') "-UserSid that is not a SID is refused"
$r = Resolve-AclUserSid "Q:\OpenRGB2" $u2
Check ($r.Sid -eq $u2) "-UserSid wins over resolution"

#====================================================================#
Section "T15 start: findings on stdout + exit contract; log rotation by rename"
#====================================================================#
# Log rotation: a log nobody holds is RENAMED to .1 (the previous .1
# replaced); a held log is left alone; a small log is never touched.
$logDir = Join-Path $TestRoot "logrot"
New-Item -ItemType Directory -Force $logDir | Out-Null
$big = Join-Path $logDir "startup.log"
[IO.File]::WriteAllText($big, ("x" * 2048))
[IO.File]::WriteAllText("$big.1", "older")
Invoke-LogRotation $big 1024
Check ((-not (Test-Path $big)) -and (Get-Item "$big.1").Length -eq 2048) `
    "unheld log past the limit is renamed to .1 (previous .1 replaced), not truncated"
[IO.File]::WriteAllText($big, ("y" * 2048))
$hold = [IO.File]::Open($big, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
try { Invoke-LogRotation $big 1024 } finally { $hold.Dispose() }
Check ((Get-Item $big).Length -eq 2048 -and (Get-Item "$big.1").Length -eq 2048 -and
       (Get-Content -Raw "$big.1").StartsWith("x")) `
    "a log someone holds open is left alone (no truncation, no rename)"
$small = Join-Path $logDir "shutdown.log"
[IO.File]::WriteAllText($small, "short")
Invoke-LogRotation $small 1024
Check ((Test-Path $small) -and -not (Test-Path "$small.1")) "a log under the limit is not rotated"

# Output contract (the UI parses the start script's STDOUT): run the
# real script as a separate -File process over a throwaway install,
# -ScanOnly (spawns nothing). Whatever this machine's live process
# table holds, the findings land on stdout and the exit code matches
# them: UNVERIFIABLE -> 4, an ownership CONFLICT -> 3, else 0.
$scanInst = Join-Path $TestRoot "install-scan"
New-Item -ItemType Directory -Force (Join-Path $scanInst "backend") | Out-Null
foreach($f in @("openrgb2-engine.exe", "backend\OpenRGB.exe")) {
    [IO.File]::WriteAllText((Join-Path $scanInst $f), "")
}
$stdout = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $ScriptsDir "start-openrgb2.ps1") `
    -InstallDir $scanInst -ScanOnly 2>$null
$scanRc = $LASTEXITCODE
$text = ($stdout -join "`n")
Check ($text -match "OpenRGB2 managed startup" -and
       $text -match "ownership (scan: no competing|CONFLICT:|notice)") `
    "start -File: the ownership scan reaches stdout"
$expectRc = if($text -match "UNVERIFIABLE:") { 4 }
            elseif($text -match "ownership CONFLICT:") { 3 } else { 0 }
Check ($scanRc -eq $expectRc) "start -File exit code $scanRc matches its stdout findings (expected $expectRc)"

#====================================================================#
Section "T16 start -Elevated: forwards -ScanOnly, returns the elevated exit code"
#====================================================================#
# The UAC re-launch used to drop -ScanOnly (the elevated copy started
# the whole stack) and always returned 0 without waiting (audit
# P-14). Start-Process is stubbed: no UAC prompt, nothing spawned.
$script:ElevCall = $null
$rcElev = & {
    function Start-Process {
        param([string]$FilePath, [string]$Verb, [object]$ArgumentList,
              [switch]$PassThru, [switch]$Wait, [string]$WindowStyle)
        $script:ElevCall = [pscustomobject]@{
            File = $FilePath; Verb = $Verb; Args = (@($ArgumentList) -join ' ')
            PassThru = [bool]$PassThru; Waited = $false }
        $fake = [pscustomobject]@{ ExitCode = 3 }
        $fake | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {
            $script:ElevCall.Waited = $true }
        if($PassThru) { return $fake }
    }
    Invoke-ManagedStart -InstallDir $instA -Scene "" -NoUI $true `
        -TakeOver $false -ScanOnly $true -Elevated $true `
        -BackendWaitSeconds 1 -ListenerPidOverride 0
}
Check ($null -ne $script:ElevCall -and $script:ElevCall.Verb -eq "RunAs") "-Elevated re-launches via RunAs"
Check ($null -ne $script:ElevCall -and $script:ElevCall.Args -match '(^|\s)-ScanOnly(\s|$)') `
      "-Elevated forwards -ScanOnly to the elevated copy"
Check ($null -ne $script:ElevCall -and $script:ElevCall.Waited) "-Elevated waits for the elevated copy"
Check (@($rcElev).Count -eq 1 -and $rcElev -eq 3) "-Elevated returns the elevated copy's exit code (3)"

} catch {
    $script:Fail++
    Write-Host "  FAIL unhandled exception: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "    at $($_.InvocationInfo.PositionMessage)" -ForegroundColor Red
} finally {
    Kill-Spawned
    Remove-Item Env:\OPENRGB2_TEST_ENGINE_PID, Env:\OPENRGB2_TEST_BACKEND_PID `
        -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $TestRoot
}

#====================================================================#
Write-Host ""
Write-Host ("RESULT: {0} passed, {1} failed" -f $script:Pass, $script:Fail)
exit $script:Fail
