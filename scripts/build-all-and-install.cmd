@echo off
rem Double-clickable shim for build-all-and-install.ps1 -- the script
rem builds as you and asks UAC only for the steps that need admin
rem (stopping/deploying the elevated install), so do not "run as
rem administrator".
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-all-and-install.ps1" %*
echo.
echo Exit code: %ERRORLEVEL%
pause
