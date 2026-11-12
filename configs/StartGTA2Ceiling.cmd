@echo off
setlocal
cd /d "%~dp0"
if not exist "Z:\tmp\xr" mkdir "Z:\tmp\xr"
>"Z:\tmp\xr\version" echo 0.6
if not exist "gta2.exe" (
  echo Put this launcher beside your own verified GTA2 v9.6 executable.
  pause
  exit /b 2
)
if not exist "gta2ceiling.dll" exit /b 2
if not exist "player" mkdir "player"
if exist "gta2_wine_settings.reg" regedit /S "gta2_wine_settings.reg"
regedit /S "install.reg"
start /wait "" "gta2.exe"
endlocal
