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
      T6  rollback: manifest-verified restore of previous binaries,
          post-update user state preserved, corrupt backup refused
      T7  clean shutdown ordering: IPC QUIT reaches the engine while
          the backend is still up, both exit, profile file untouched
      T8  update refuses while the install is running -- engine AND
          UI (image-path verified); rollback refuses too
      T9  backend guard: a foreign OpenRGB.exe refuses the spawn even
          under -TakeOver (the pinned backend's own kill-capable
          single-instance prompt is never triggered); an OpenRGB.exe
          at OUR pinned path mid-start is waited on, never
          double-spawned

    Exit code: number of failed checks.
#>
[CmdletBinding()]
param(
    # packaging/windows source dir holding the scripts under test.
    [string]$ScriptsDir = "$PSScriptRoot\..\..\packaging\windows"
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
    [pscustomobject]@{ pid=54;  exe="Steam.exe" },          # unrelated -- invisible
    [pscustomobject]@{ pid=999; exe="LConnect.exe" }        # self pid -- skipped
)
$f = Get-OwnerFindings -Processes $procs -SelfPid 999 -BackendPid 7
$conflicts = @($f | Where-Object conflict)
$notices   = @($f | Where-Object { -not $_.conflict })
Check ($conflicts.Count -eq 4) "conflicts: other-OpenRGB + LConnect + iCUE + Synapse"
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
Check ([regex]::Matches($stopSrc, 'Stop-Process\s+-').Count -eq 2 -and
       [regex]::Matches($stopSrc, '\$Force -and \(Test-OwnProcess').Count -eq 2) `
      "stop script Stop-Process only behind -Force + own-process verification"

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

$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instB -SkipVerify $false
Check ($rc -eq 0) "update returns 0"
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
$rc = Invoke-PackageRollback -InstallDir $instB -BackupDir "" -SkipVerify $false
Check ($rc -eq 1) "corrupt backup refused"
Check ((Read-Tag $instB) -eq "V2") "refused rollback changed nothing"
Set-Content -Encoding ASCII "$instB.previous\obsolete.dll" "old-binary"

$rc = Invoke-PackageRollback -InstallDir $instB -BackupDir "" -SkipVerify $false
Check ($rc -eq 0) "rollback returns 0"
Check ((Read-Tag $instB) -eq "V1") "previous binaries restored"
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
    -Force $true -CtlExe $ctlStub
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

# nothing-running stop is a clean no-op
$rc = Invoke-ManagedStop -InstallDir $instC -TimeoutSeconds 1 `
    -Force $false -CtlExe $ctlStub
Check ($rc -eq 0) "stop with nothing running exits 0"

#====================================================================#
Section "T8 update refuses while install is running"
#====================================================================#
$instD = Join-Path $TestRoot "install-d"
New-FakeTree $instD "1.0.0" "D" (Join-Path $Sys32 "cmd.exe") (Join-Path $Sys32 "cmd.exe")
$running = Start-Process -FilePath (Join-Path $instD "openrgb2-engine.exe") -PassThru
$script:Spawned += $running.Id
Start-Sleep -Milliseconds 500
$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instD -SkipVerify $true
Check ($rc -eq 1) "update refused while engine running"
Check ((Read-Tag $instD) -eq "D") "refused update changed nothing"
Check (-not (Test-Path "$instD.previous")) "refused update took no snapshot"
Kill-Spawned

# a running UI locks openrgb2-ui.exe -> update must refuse just the
# same (review fix r1: the check used to watch engine+backend only).
$ui = Start-Process -FilePath (Join-Path $instD "openrgb2-ui.exe") -PassThru
$script:Spawned += $ui.Id
Start-Sleep -Milliseconds 500
$rc = Invoke-PackageUpdate -PackageDir $pkgV2 -InstallDir $instD -SkipVerify $true
Check ($rc -eq 1) "update refused while UI running"
Check ((Read-Tag $instD) -eq "D") "UI-blocked update changed nothing"
# rollback refuses while running too (needs a valid backup present
# first -- the running check must fire before any restore).
New-FakeTree "$instD.previous" "0.9.0" "D0" `
    (Join-Path $Sys32 "cmd.exe") (Join-Path $Sys32 "cmd.exe")
$rc = Invoke-PackageRollback -InstallDir $instD -BackupDir "" -SkipVerify $true
Check ($rc -eq 1) "rollback refused while UI running"
Check ((Read-Tag $instD) -eq "D") "refused rollback changed nothing"
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
Write-Host ""
Write-Host ("RESULT: {0} passed, {1} failed" -f $script:Pass, $script:Fail)
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $TestRoot
exit $script:Fail
