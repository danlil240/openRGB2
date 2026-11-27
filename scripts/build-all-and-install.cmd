@echo off
rem Double-clickable shim for build-all-and-install.ps1 -- the script
rem self-elevates via UAC, so no "run as administrator" needed.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-all-and-install.ps1" %*
echo.
echo Exit code: %ERRORLEVEL%
pause
