@echo off
rem Double-click: build, install and start GTA2Ceiling on the USB-connected Quest.
rem Log: <work root>\implementation\logs\deploy_quest.log (+ .status); the work root is
rem GTA2CEILING_WORK_ROOT, or D:\GTA2Ceiling-work by default.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_quest.ps1" %*
if errorlevel 1 (echo FAILED) else (echo OK - GTA2Ceiling started on the Quest)
pause
