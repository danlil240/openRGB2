@echo off
rem OpenRGB2 managed startup (Task 6.2) -- double-click entry point.
rem Runs the full startup flow: ownership scan -> explicit takeover
rem gate -> pinned backend -> engine -> UI. See INSTALL.md.
rem Args are passed through, e.g.:  OpenRGB2.cmd -TakeOver -NoUI
rem Each arg is re-quoted individually: a bare %* lets cmd metachars
rem inside an argument corrupt this line, while "%*" would collapse
rem the whole tail into ONE PowerShell argument (switches never bind).
rem In a staged release the script is under scripts\. From this source
rem directory, launch the staged release so its binaries/config stay
rem together with that script: the NEWEST staged package by last write
rem time, the same pick stop-openrgb2.ps1 makes from here.
set "ORGB2_START=%~dp0scripts\start-openrgb2.ps1"
if exist "%ORGB2_START%" goto :orgb2_args
set "ORGB2_START="
set "ORGB2_PKG=%~dp0..\..\build\package"
for /f "delims=" %%D in ('dir /b /ad /o-d /tw "%ORGB2_PKG%\OpenRGB2-*-win64" 2^>nul') do (
    if exist "%ORGB2_PKG%\%%D\scripts\start-openrgb2.ps1" (
        for %%F in ("%ORGB2_PKG%\%%D\scripts\start-openrgb2.ps1") do set "ORGB2_START=%%~fF"
        goto :orgb2_args
    )
)
echo ERROR: No staged OpenRGB2 release found under build\package.
echo Stage a release with packaging\windows\stage-release.ps1, then retry.
pause
exit /b 1
:orgb2_args
set "ORGB2_ARGS="
:orgb2_next
if "%~1"=="" goto :orgb2_run
set "ORGB2_ARGS=%ORGB2_ARGS% "%~1""
shift
goto :orgb2_next
:orgb2_run
powershell -NoProfile -ExecutionPolicy Bypass -File "%ORGB2_START%" %ORGB2_ARGS%
rem Keep the script's exit code (3 = ownership refusal, 4 = elevated
rem stack unverifiable) for callers -- pause would otherwise reset it.
set "ORGB2_RC=%ERRORLEVEL%"
pause
exit /b %ORGB2_RC%
