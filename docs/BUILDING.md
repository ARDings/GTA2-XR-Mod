# Building

Windows only for now. You need Visual Studio 2022 (MSVC, x86 toolset, CMake), Python 3.12+ and, for the host APK, the Android NDK r27c/SDK/Gradle that ship with Unity 6000.3. Tool paths are script parameters; machine-specific overrides go into `tools/local.settings.ps1` (not committed).

## Work root

Nothing is built inside the source tree. All outputs, caches and logs go to the work root:

- `D:\GTA2Ceiling-work` by default, or
- the folder in `GTA2CEILING_WORK_ROOT`, e.g. `setx GTA2CEILING_WORK_ROOT E:\gta2ceiling-work`.

CMake and the scripts refuse other locations.

## Mod DLLs

```powershell
.\tools\build_mod.ps1
```

Builds `gta2ceiling.dll` and `gta2ceiling_vid.dll` (x86), runs the C++ and Python tests and creates a mod package without any game data.

## Host APK

See [runtime/winlatorxr-patches/README.md](../runtime/winlatorxr-patches/README.md): a pinned WinlatorXR checkout, the OpenXR loader AAR, then `BuildHost.ps1`.

## Onto the Quest

- `tools\deploy_quest.cmd`: builds mod and host, installs both on the USB-connected Quest and starts the game.
- `tools\collect_quest_logs.cmd`: pulls logs into `<work root>\implementation\logs\quest`.
