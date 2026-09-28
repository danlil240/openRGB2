<#
.SYNOPSIS
    Stage the OpenRGB2 release package for Windows (Task 6.1).

.DESCRIPTION
    Reproducibly assembles the release tree under <OutDir>:

      <OutDir>/
        openrgb2-ui.exe        + windeployqt -qmldir output (Qt runtime + QML)
        openrgb2-engine.exe    Qt-free; MSVC CRT copied in
        openrgb2-ctl.exe       Qt-free; MSVC CRT copied in
        presets/devices|effects   bundled device/effect JSON library
        profiles/                 committed topology profiles (no unique IDs)
        backend/OpenRGB.exe    the PINNED vendored backend + windeployqt
                               output + non-Qt payloads (PawnIO, hidapi,
                               libusb, *.bin firmware blobs)
        licenses/              full license texts
        NOTICES.txt            license/provenance manifest
        scripts/*.ps1          managed lifecycle (Task 6.2): start /
                               stop / update / rollback
        OpenRGB2.cmd           double-click launcher -> scripts\start
        OpenRGB2.vbs + .ico    hidden desktop launcher and icon
        INSTALL.md             install/startup/recovery guide
        manifest.json          source/build manifest (per-file SHA-256)

    After staging, a verification pass asserts required files are present
    and forbidden components are absent (old effect plugins, the
    WinRing0-based HardwareSync dependency, the L-Connect yuz.dll,
    debug Qt DLLs, backend user state).

    The backend binary is taken ONLY from the vendored fork's own build
    output (vendor/OpenRGB/"OpenRGB Windows 64-bit" or vendor/OpenRGB/release)
    -- never from an installed system OpenRGB.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File packaging\windows\stage-release.ps1

.EXAMPLE
    # Re-check an already staged tree
    powershell -File packaging\windows\stage-release.ps1 -VerifyOnly -OutDir build\package\OpenRGB2-0.1.0-win64
#>
[CmdletBinding()]
param(
    # CMake build tree that contains src/*/Release and tools/*/Release outputs.
    [string]$BuildDir,

    # Package output directory. Default: <build>\package\OpenRGB2-<version>-win64
    [string]$OutDir,

    # Build configuration of the inputs.
    [string]$Configuration = "Release",

    # Qt installation root (contains bin\windeployqt.exe).
    # Falls back to $env:OPENRGB2_QT_DIR, then the pinned toolchain path.
    [string]$QtDir,

    # Vendored backend deploy dir containing OpenRGB.exe + its payload.
    # Falls back to $env:OPENRGB2_BACKEND_DIR, then the bat recipe output,
    # then vendor/OpenRGB/release (mid-recipe state).
    [string]$BackendDeployDir,

    # MSVC redist CRT folder (contains vcruntime140.dll etc.).
    # Falls back to $env:OPENRGB2_VC_REDIST, then the newest
    # VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT under BuildTools.
    [string]$VcRedistDir,

    # Skip re-staging; only verify an existing -OutDir.
    [switch]$VerifyOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

#--------------------------------------------------------------------#
# Resolve roots                                                       #
#--------------------------------------------------------------------#
$PkgDir   = Split-Path -Parent $MyInvocation.MyCommand.Path          # packaging/windows
$RepoRoot = (Resolve-Path (Join-Path $PkgDir "..\..")).Path

if(-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "build" }
$BuildDir = (Resolve-Path $BuildDir).Path

# Version from the top-level CMakeLists (project(OpenRGB2 VERSION x.y.z)).
$cmakeTop = Get-Content (Join-Path $RepoRoot "CMakeLists.txt") -Raw
$version  = "0.0.0"
if($cmakeTop -match "project\s*\(\s*OpenRGB2\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)") {
    $version = $Matches[1]
}

if(-not $OutDir) {
    $OutDir = Join-Path $BuildDir "package\OpenRGB2-$version-win64"
}

if(-not $QtDir) {
    if($env:OPENRGB2_QT_DIR) { $QtDir = $env:OPENRGB2_QT_DIR }
    else                   { $QtDir = "C:\Qt\6.8.3\msvc2022_64" }
}
$windeployqt = Join-Path $QtDir "bin\windeployqt.exe"

if(-not $BackendDeployDir) {
    if($env:OPENRGB2_BACKEND_DIR) {
        $BackendDeployDir = $env:OPENRGB2_BACKEND_DIR
    } else {
        $BackendDeployDir = Join-Path $RepoRoot "vendor\OpenRGB\OpenRGB Windows 64-bit"
        if(-not (Test-Path (Join-Path $BackendDeployDir "OpenRGB.exe"))) {
            $BackendDeployDir = Join-Path $RepoRoot "vendor\OpenRGB\release"
        }
    }
}

if(-not $VcRedistDir) {
    if($env:OPENRGB2_VC_REDIST) {
        $VcRedistDir = $env:OPENRGB2_VC_REDIST
    } else {
        $redistRoot = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"
        $newest = Get-ChildItem $redistRoot -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match '^\d+\.\d+' } |
                  Sort-Object { [version]($_.Name -replace '[^0-9\.]','') } -Descending |
                  Select-Object -First 1
        if($newest) {
            $VcRedistDir = Join-Path $newest.FullName "x64\Microsoft.VC143.CRT"
        }
    }
}

#--------------------------------------------------------------------#
# Small helpers                                                       #
#--------------------------------------------------------------------#
function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Info([string]$msg) { Write-Host "  $msg" }
function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

function Require([string]$path, [string]$what) {
    if(-not (Test-Path $path)) { Fail "$what not found: $path" }
    return (Resolve-Path $path).Path
}

function Copy-Tree([string]$src, [string]$dst) {
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item -Recurse -Force -Path (Join-Path $src "*") -Destination $dst
}

# Scrub private path prefixes before they land in the shipped
# manifest.json: repo-local paths become <repo>-relative and anything
# under the build user's profile collapses to <profile>, so the package
# never leaks a local username (same redaction class as the docs pass).
function ConvertTo-ManifestPath([string]$p) {
    if(-not $p) { return $p }
    $norm = $p -replace '/','\'
    if($norm.StartsWith($RepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return ('<repo>' + $norm.Substring($RepoRoot.Length))
    }
    $prof = [Environment]::GetFolderPath('UserProfile')
    if($prof -and $norm.StartsWith($prof, [StringComparison]::OrdinalIgnoreCase)) {
        return ('<profile>' + $norm.Substring($prof.Length))
    }
    return $norm
}

#--------------------------------------------------------------------#
# Input binaries                                                      #
#--------------------------------------------------------------------#
$uiExe     = Join-Path $BuildDir "src\ui\$Configuration\openrgb2-ui.exe"
$engineExe = Join-Path $BuildDir "src\engine\$Configuration\openrgb2-engine.exe"
$ctlExe    = Join-Path $BuildDir "tools\engine_ctl\$Configuration\openrgb2-ctl.exe"
$backendExe = Join-Path $BackendDeployDir "OpenRGB.exe"

if(-not $VerifyOnly) {
    Require $uiExe     "UI executable (build it: cmake --build build --config $Configuration)" | Out-Null
    Require $engineExe "engine executable" | Out-Null
    Require $ctlExe    "openrgb2-ctl executable" | Out-Null
    Require $backendExe "pinned vendored backend (build: vendor\OpenRGB\scripts\build-windows.bat)" | Out-Null
    Require $windeployqt "windeployqt (QtDir '$QtDir')" | Out-Null
    if(-not $VcRedistDir -or -not (Test-Path (Join-Path $VcRedistDir "vcruntime140.dll"))) {
        Fail "MSVC redist CRT not found (set -VcRedistDir or OPENRGB2_VC_REDIST)"
    }
    Require (Join-Path $RepoRoot "src\core\presets\devices") "bundled device presets" | Out-Null
    Require (Join-Path $RepoRoot "src\core\presets\effects") "bundled effect presets" | Out-Null
    Require (Join-Path $PkgDir "NOTICES.txt") "notices manifest source" | Out-Null
    Require (Join-Path $PkgDir "licenses")    "license texts" | Out-Null
    foreach($s in @("start-openrgb2.ps1","start-openrgb2-gui.ps1",
                    "stop-openrgb2.ps1","update-openrgb2.ps1",
                    "rollback-openrgb2.ps1","create-desktop-shortcut.ps1",
                    "OpenRGB2.cmd","OpenRGB2.vbs","OpenRGB2.ico",
                    "install-recovery.md","RELEASE.md")) {
        Require (Join-Path $PkgDir $s) "managed-lifecycle file $s" | Out-Null
    }

    Step "Staging OpenRGB2 $version -> $OutDir"
    if(Test-Path $OutDir) {
        # A running staged stack holds its payload DLLs locked -- refuse
        # the wipe with a pointer at the managed stop instead of dying
        # mid-delete on UnauthorizedAccess. Elevated processes hide
        # their image path from a non-elevated Get-Process, so also
        # match by executable name (conservative: flags any same-named
        # process even when the path can't be verified as ours).
        $outFull  = (Resolve-Path $OutDir).Path
        $locking  = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and $_.Path.StartsWith($outFull,
                    [StringComparison]::OrdinalIgnoreCase) }
            catch { $false }
        })
        $locking += @(Get-Process -Name "openrgb2-engine","openrgb2-ui",
                      "openrgb2-ctl","OpenRGB" -ErrorAction SilentlyContinue |
            Where-Object { $locking.Id -notcontains $_.Id })
        if($locking.Count -gt 0) {
            Fail ("staged binaries are still running (" +
                  (($locking | ForEach-Object { "$($_.ProcessName)#$($_.Id)" }) -join ', ') +
                  ") -- stop them first: scripts\stop-openrgb2.ps1 " +
                  "(elevated instances need an elevated shell)")
        }
        # Retry past transient locks (AV/indexer), then give up cleanly.
        $wiped = $false
        foreach($try_ in 1..3) {
            try { Remove-Item -Recurse -Force $OutDir -ErrorAction Stop; $wiped = $true; break }
            catch { Start-Sleep -Seconds 2 }
        }
        if(-not $wiped) {
            Fail "could not clear $OutDir -- a process still holds a file/dir handle inside it (check for an elevated OpenRGB2 stack or a shell parked in that directory)"
        }
    }
    New-Item -ItemType Directory -Force -Path $OutDir          | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "backend") | Out-Null

    #--------------------------------------------------------------#
    # Application binaries                                          #
    #--------------------------------------------------------------#
    Copy-Item $uiExe     (Join-Path $OutDir "openrgb2-ui.exe")
    Copy-Item $engineExe (Join-Path $OutDir "openrgb2-engine.exe")
    Copy-Item $ctlExe    (Join-Path $OutDir "openrgb2-ctl.exe")
    Info "application executables copied"

    #--------------------------------------------------------------#
    # Qt runtime for the UI (windeployqt scans ui/*.qml imports)    #
    #--------------------------------------------------------------#
    & $windeployqt --release `
        --qmldir (Join-Path $RepoRoot "ui") `
        --no-translations --no-system-d3d-compiler `
        --no-compiler-runtime --no-opengl-sw --no-network `
        (Join-Path $OutDir "openrgb2-ui.exe") | Out-Null
    if($LASTEXITCODE -ne 0) { Fail "windeployqt failed for openrgb2-ui.exe" }
    # --no-network skips Qt6Network.dll, but Qt6Qml/Qt6Quick STATICALLY
    # import it -- without this copy the staged UI dies at loader init
    # (0xC0000135, "Qt6Network.dll was not found"). Shipping the lib
    # adds no listener surface by itself.
    Copy-Item (Require (Join-Path $QtDir "bin\Qt6Network.dll") "Qt6Network.dll") $OutDir
    Info "Qt runtime + QML modules deployed for UI (+Qt6Network.dll)"

    #--------------------------------------------------------------#
    # MSVC CRT runtime (engine/ctl have no windeployqt step)        #
    #--------------------------------------------------------------#
    Copy-Item (Join-Path $VcRedistDir "*.dll") $OutDir
    Copy-Item (Join-Path $VcRedistDir "*.dll") (Join-Path $OutDir "backend")
    Info "MSVC CRT runtime copied (root + backend/)"

    #--------------------------------------------------------------#
    # Pinned vendored backend                                       #
    #--------------------------------------------------------------#
    $backendDir = Join-Path $OutDir "backend"
    Copy-Item $backendExe (Join-Path $backendDir "OpenRGB.exe")

    # Fresh Qt deployment matching the backend's own recipe
    # (vendor/OpenRGB/scripts/build-windows.bat windeployqt flags).
    & $windeployqt --release --no-patchqt --no-quick-import `
        --no-translations --no-system-d3d-compiler `
        --no-compiler-runtime --no-opengl-sw --no-network `
        (Join-Path $backendDir "OpenRGB.exe") | Out-Null
    if($LASTEXITCODE -ne 0) { Fail "windeployqt failed for OpenRGB.exe" }

    # The recipe's manual extras (kept for parity with the pinned tree).
    foreach($dll in @("Qt6Core5Compat.dll", "Qt6OpenGL.dll", "Qt6Svg.dll")) {
        $src = Join-Path $QtDir "bin\$dll"
        $dst = Join-Path $backendDir $dll
        if((Test-Path $src) -and -not (Test-Path $dst)) { Copy-Item $src $dst }
    }

    # Non-Qt payloads pinned in the vendored deploy dir: hardware-access
    # DLLs and PawnIO firmware modules (*.bin). Top-level only --
    # Qt plugin subdirs are re-created by windeployqt above.
    $nonQt = Get-ChildItem $BackendDeployDir -File |
        Where-Object {
            ($_.Extension -eq ".bin") -or
            ($_.Extension -eq ".dll" -and $_.Name -notmatch "^Qt6")
        }
    foreach($f in $nonQt) { Copy-Item $f.FullName $backendDir }
    Info "pinned backend staged from $BackendDeployDir"

    #--------------------------------------------------------------#
    # Bundled preset library + committed profiles                   #
    #--------------------------------------------------------------#
    Copy-Tree (Join-Path $RepoRoot "src\core\presets") (Join-Path $OutDir "presets")
    foreach($p in @("devices", "effects")) {   # drop build-system sources
        Remove-Item -Force -ErrorAction SilentlyContinue `
            (Join-Path $OutDir "presets\$p\*.cpp"), (Join-Path $OutDir "presets\$p\*.h")
    }
    Remove-Item -Force -ErrorAction SilentlyContinue `
        (Join-Path $OutDir "presets\*.cpp"), (Join-Path $OutDir "presets\*.h")
    # Stage committed topology profiles whenever the dir holds at
    # least one *.json -- not keyed on a specific filename, so a
    # profile rename can't silently drop them.
    $profilesSrc  = Join-Path $RepoRoot "profiles"
    $profileFiles = @(Get-ChildItem $profilesSrc -Filter *.json -File -ErrorAction SilentlyContinue)
    if($profileFiles.Count -ge 1) {
        Copy-Tree $profilesSrc (Join-Path $OutDir "profiles")
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $OutDir "profiles\local")
        Info "preset library + committed profiles staged ($($profileFiles.Count) profile(s))"
    } else {
        Write-Host "WARNING: no committed profiles under $profilesSrc -- package ships without profiles/" -ForegroundColor Yellow
        Info "preset library staged"
    }

    #--------------------------------------------------------------#
    # Notices + licenses                                            #
    #--------------------------------------------------------------#
    Copy-Item (Join-Path $PkgDir "NOTICES.txt") $OutDir
    Copy-Tree (Join-Path $PkgDir "licenses") (Join-Path $OutDir "licenses")
    Info "NOTICES.txt + license texts staged"

    #--------------------------------------------------------------#
    # Managed lifecycle (Task 6.2): startup/stop/update/rollback    #
    # scripts under scripts/, the double-click launcher at the      #
    # root, and the install/recovery guide as INSTALL.md.           #
    #--------------------------------------------------------------#
    New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "scripts") | Out-Null
    foreach($s in @("start-openrgb2.ps1","start-openrgb2-gui.ps1",
                    "stop-openrgb2.ps1","update-openrgb2.ps1",
                    "rollback-openrgb2.ps1","create-desktop-shortcut.ps1")) {
        Copy-Item (Join-Path $PkgDir $s) (Join-Path $OutDir "scripts")
    }
    Copy-Item (Join-Path $PkgDir "OpenRGB2.cmd") $OutDir
    Copy-Item (Join-Path $PkgDir "OpenRGB2.vbs") $OutDir
    Copy-Item (Join-Path $PkgDir "OpenRGB2.ico") $OutDir
    Copy-Item (Join-Path $PkgDir "install-recovery.md") `
              (Join-Path $OutDir "INSTALL.md")
    Copy-Item (Join-Path $PkgDir "RELEASE.md") $OutDir
    Info "managed-lifecycle scripts + INSTALL.md staged"

    #--------------------------------------------------------------#
    # Source/build manifest -- collected now, but only WRITTEN after #
    # the verification pass below succeeds, so a failed package can  #
    # never carry an "exclusions_verified" claim.                    #
    #--------------------------------------------------------------#
    $gitRev = ""
    try { $gitRev = (& git -C $RepoRoot rev-parse HEAD 2>$null).Trim() } catch {}

    $files = @()
    Get-ChildItem $OutDir -Recurse -File |
        Where-Object { $_.Name -ne "manifest.json" } |
        ForEach-Object {
            $rel = ($_.FullName.Substring($OutDir.Length).TrimStart('\','/') -replace '\\','/')
            $origin =
                if($rel -like "backend/Qt6*" -or $rel -match "^backend/(platforms|styles|imageformats|iconengines)/") {
                    "qt-runtime" }
                elseif($_.Name -match "^(vcruntime|msvcp|concrt|vccorlib)") {
                    "msvc-crt" }
                elseif($rel -like "backend/*")      { "vendored-backend" }
                elseif($rel -like "presets/*")      { "src/core/presets" }
                elseif($rel -like "profiles/*")     { "profiles" }
                elseif($rel -like "licenses/*")     { "packaging/windows/licenses" }
                elseif($rel -like "scripts/*")      { "packaging/windows" }
                elseif($rel -eq "OpenRGB2.ico")  { "vendor/OpenRGB/qt/OpenRGB.ico" }
                elseif($rel -eq "NOTICES.txt" -or $rel -eq "OpenRGB2.cmd" -or
                       $rel -eq "OpenRGB2.vbs" -or
                       $rel -eq "INSTALL.md" -or $rel -eq "RELEASE.md") {
                                                       "packaging/windows" }
                elseif($_.Name -like "Qt6*.dll" -or
                       $_.FullName -match "platforms|styles|imageformats|iconengines|qml|qmltooling|translations|tls|generic|networkinformation") {
                    "qt-runtime" }
                else { "app-build" }
            $files += [ordered]@{
                path   = $rel
                sha256 = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower()
                bytes  = $_.Length
                origin = $origin
            }
        }

    $manifest = [ordered]@{
        package      = "OpenRGB2"
        version      = $version
        platform     = "windows-x64"
        generated_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        source = [ordered]@{
            repository   = "openRGB2"
            revision     = $gitRev
            build_dir    = ConvertTo-ManifestPath $BuildDir
            configuration= $Configuration
            qt_dir       = ConvertTo-ManifestPath $QtDir
            backend_dir  = ConvertTo-ManifestPath $BackendDeployDir
            backend_pin  = "vendor/OpenRGB @ 53a477eab8d14b0bc49aeb40e3d569cbcb16a377 (lianli-wireless; see vendor/provenance/MANIFEST.md)"
            vc_redist    = ConvertTo-ManifestPath $VcRedistDir
        }
        exclusions_verified = @(
            "old effect plugins (plugins/ dirs, *EffectPlugin*)",
            "WinRing0-based HardwareSync dependency",
            "installed L-Connect yuz.dll"
        )
        files = $files
    }
    Info "manifest content collected ($($files.Count) files) -- written after verification"
}

#--------------------------------------------------------------------#
# Verification (always runs -- this is the exclusion assert)          #
#--------------------------------------------------------------------#
Step "Verifying package at $OutDir"
if(-not (Test-Path $OutDir)) { Fail "package dir does not exist" }

$required = @(
    "openrgb2-ui.exe",
    "openrgb2-engine.exe",
    "openrgb2-ctl.exe",
    "Qt6Core.dll",
    "Qt6Network.dll",
    "Qt6QuickControls2.dll",
    "platforms\qwindows.dll",
    "backend\OpenRGB.exe",
    "backend\Qt6Widgets.dll",
    "backend\platforms\qwindows.dll",
    "backend\libusb-1.0.dll",
    "backend\hidapi-hotplug.dll",
    "backend\PawnIOLib.dll",
    "presets\devices",
    "presets\effects",
    "NOTICES.txt",
    "licenses",
    "scripts\start-openrgb2.ps1",
    "scripts\start-openrgb2-gui.ps1",
    "scripts\stop-openrgb2.ps1",
    "scripts\update-openrgb2.ps1",
    "scripts\rollback-openrgb2.ps1",
    "scripts\create-desktop-shortcut.ps1",
    "OpenRGB2.cmd",
    "OpenRGB2.vbs",
    "OpenRGB2.ico",
    "INSTALL.md",
    "RELEASE.md"
)
# manifest.json is only written AFTER this verification passes (see
# below); when re-checking an existing tree it must already be there.
if($VerifyOnly) { $required += "manifest.json" }
$missing = @()
foreach($r in $required) {
    if(-not (Test-Path (Join-Path $OutDir $r))) { $missing += $r }
}
if($missing.Count -gt 0) {
    Fail "missing required package files: $($missing -join ', ')"
}

# QML modules must have been deployed for the UI's QtQuick imports.
if(-not (Test-Path (Join-Path $OutDir "qml"))) {
    Fail "qml/ module directory missing -- windeployqt -qmldir did not run?"
}
if((Get-ChildItem (Join-Path $OutDir "presets\devices") -Filter *.json).Count -eq 0) {
    Fail "presets/devices is empty"
}
if((Get-ChildItem (Join-Path $OutDir "presets\effects") -Filter *.json).Count -eq 0) {
    Fail "presets/effects is empty"
}

# Forbidden components -- the plan's explicit exclusions.
$all = Get-ChildItem $OutDir -Recurse -Force
$forbidden = @()

# 1. Old effect plugins: any plugins/ directory (OpenRGB's plugin
#    loader location) or plugin DLLs outside Qt's qml/ tree (Qt's own
#    QML plugin DLLs such as effectsplugin.dll legitimately live under
#    qml\ only). Also the app-side plugin names outright.
$forbidden += $all | Where-Object {
    $_.PSIsContainer -and $_.Name -match '^(plugins|OpenRGBPlugins?)$'
}
$qtPluginDirs = '\\(qml|qmltooling|generic|platforminputcontexts|' +
                'platforms|imageformats|iconengines|styles|tls|' +
                'networkinformation|scenegraph|multimedia|sqldrivers)\\'
$forbidden += $all | Where-Object {
    -not $_.PSIsContainer -and
    $_.Name -match '(?i)plugin.*\.dll$' -and
    $_.FullName -notmatch $qtPluginDirs
}
$forbidden += $all | Where-Object {
    -not $_.PSIsContainer -and
    $_.Name -match '(?i)(HardwareSync|OpenRGBPlugin|LightingStudio)'
}
# 2. WinRing0 driver/dependency.
$forbidden += $all | Where-Object { $_.Name -match '(?i)^WinRing0' }
# 3. Installed L-Connect yuz.dll (tinyuz is compiled in statically --
#    no DLL must ship).
$forbidden += $all | Where-Object { -not $_.PSIsContainer -and $_.Name -match '(?i)^yuz\.dll$' }
# 4. Backend user state / logs must not leak into the package.
$forbidden += $all | Where-Object {
    $_.Name -match '^(OpenRGB\.json|.*\.log)$' -and $_.FullName -match '\\backend\\'
}
# 4b. Gitignored user-local profiles must not leak into the package
#     (same class of assert as backend\OpenRGB.json above).
$forbidden += $all | Where-Object {
    $_.FullName -match '\\profiles\\local(\\|$)'
}
# 5. Debug Qt runtime DLLs. Qt debug builds are '<module>d.dll'
#    (Qt6Cored.dll). A RELEASE Qt module whose name ends in 'd'
#    would false-positive -- allowlist such names here if ever staged.
$qtReleaseNamesEndingInD = @()   # e.g. @('Qt6Xyzd.dll') for a release DLL
$forbidden += $all | Where-Object {
    -not $_.PSIsContainer -and
    $_.Name -match '^Qt6\w*d\.dll$' -and
    $qtReleaseNamesEndingInD -notcontains $_.Name
}

if($forbidden.Count -gt 0) {
    Write-Host "FORBIDDEN package members:" -ForegroundColor Red
    $forbidden | ForEach-Object { Write-Host "  $($_.FullName)" -ForegroundColor Red }
    Fail "package contains excluded components (effect plugins / WinRing0 / yuz.dll / debug DLLs / user state)"
}

# Verification passed -- only now is it truthful to stamp the manifest
# with exclusions_verified and ship it inside the package.
if(-not $VerifyOnly) {
    ($manifest | ConvertTo-Json -Depth 6) |
        Set-Content -Encoding UTF8 (Join-Path $OutDir "manifest.json")
    if(-not (Test-Path (Join-Path $OutDir "manifest.json"))) {
        Fail "manifest.json was not written"
    }
    Info "manifest.json written ($($files.Count) files) after verification passed"
}

Write-Host "PACKAGE OK: $OutDir" -ForegroundColor Green
exit 0
