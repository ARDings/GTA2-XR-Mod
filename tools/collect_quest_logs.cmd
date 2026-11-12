@echo off
rem Pulls the Quest crash/log data for GTA2Ceiling into <work root>\implementation\logs\quest;
rem the work root is GTA2CEILING_WORK_ROOT, or D:\GTA2Ceiling-work by default.
set "ADB=C:\Program Files\Unity\Hub\Editor\6000.3.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\SDK\platform-tools\adb.exe"
if not exist "%ADB%" set "ADB=adb"
set "ROOT=D:\GTA2Ceiling-work"
if defined GTA2CEILING_WORK_ROOT set "ROOT=%GTA2CEILING_WORK_ROOT%"
set "OUT=%ROOT%\implementation\logs\quest"
if not exist "%OUT%" mkdir "%OUT%"
"%ADB%" logcat -d -v time > "%OUT%\logcat.txt" 2>&1
"%ADB%" logcat -d -b crash -v time > "%OUT%\crash.txt" 2>&1
"%ADB%" logcat -d -v time -s GTA2Pacing GTA2Cpu GTA2Reproject GTA2Occlusion GTA2Ceiling VrApi > "%OUT%\host_perf.txt" 2>&1
"%ADB%" pull /sdcard/Download/GTA2Ceiling/gta2/gta2dx9.log "%OUT%\gta2dx9.log" > nul 2>&1
"%ADB%" pull /sdcard/Download/GTA2Ceiling/gta2/gta2ceiling_present.csv "%OUT%\present.csv" > nul 2>&1
"%ADB%" pull /sdcard/Download/GTA2Ceiling/gta2/host_perf.log "%OUT%\host_perf.log" > nul 2>&1
"%ADB%" shell dumpsys package com.gta2ceiling.winlator > "%OUT%\package.txt" 2>&1
"%ADB%" shell dumpsys meminfo com.gta2ceiling.winlator > "%OUT%\meminfo.txt" 2>&1
"%ADB%" shell cat /proc/meminfo > "%OUT%\proc_meminfo.txt" 2>&1
echo done> "%OUT%\collected.txt"
echo Logs collected in %OUT%
