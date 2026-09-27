@echo off
rem OpenRGB2 managed startup (Task 6.2) — double-click entry point.
rem Runs the full startup flow: ownership scan -> explicit takeover
rem gate -> pinned backend -> engine -> UI. See INSTALL.md.
rem Args are passed through, e.g.:  OpenRGB2.cmd -TakeOver -NoUI
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-openrgb2.ps1" %*
pause
