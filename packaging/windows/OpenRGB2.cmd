@echo off
rem OpenRGB2 managed startup (Task 6.2) -- double-click entry point.
rem Runs the full startup flow: ownership scan -> explicit takeover
rem gate -> pinned backend -> engine -> UI. See INSTALL.md.
rem Args are passed through, e.g.:  OpenRGB2.cmd -TakeOver -NoUI
rem Each arg is re-quoted individually: a bare %* lets cmd metachars
rem inside an argument corrupt this line, while "%*" would collapse
rem the whole tail into ONE PowerShell argument (switches never bind).
set "ORGB2_ARGS="
:orgb2_next
if "%~1"=="" goto :orgb2_run
set "ORGB2_ARGS=%ORGB2_ARGS% "%~1""
shift
goto :orgb2_next
:orgb2_run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-openrgb2.ps1" %ORGB2_ARGS%
pause
