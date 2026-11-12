# Installing on the Quest 3

Experimental. You need your own GTA2 (version 9.6), a Quest 3 with a room scan (Meta room setup), an Xbox controller (optional: the Touch controllers work too) and the official WinlatorXR Dawn42 app installed (the GTA2Ceiling host reads its runtime assets from it, so keep it installed).

1. **Host:** build and install the GTA2Ceiling host APK ([BUILDING.md](BUILDING.md)). It is a separate app (`com.gta2ceiling.winlator`) and does not touch the original WinlatorXR.
2. **Game files:** copy your complete GTA2 folder (with `gta2.exe` and `data/`) to `/sdcard/Download/GTA2Ceiling/gta2/`. Back up your saves first.
3. **Mod files:** into the same folder copy `gta2ceiling.dll`, `gta2ceiling_vid.dll`, `install.reg` (from the mod package) and `gta2ceiling.ini`, `StartGTA2Ceiling.cmd`, `GTA2Ceiling.desktop`, `SetupWinlatorShortcut.sh` from `configs/`.
4. **Container:** in the host, create or pick a container and map Wine drive `D:` to the Quest Download folder, so the game is at `D:\GTA2Ceiling\gta2`. Start its desktop once, close it, open the container's Debug Terminal and run:
   ```sh
   sh /sdcard/Download/GTA2Ceiling/gta2/SetupWinlatorShortcut.sh
   ```
   This only writes the container's `GTA2Ceiling` shortcut.
5. **Controller:** pair the Xbox controller with the Quest via Bluetooth; it becomes player 1 by itself. The Touch controllers always work as an Xbox pad as well (see the [README](../README.md#quest-touch-controllers)).
6. **Start** the GTA2Ceiling shortcut and allow room access when asked. Without a valid room scan the game waits and shows a message.

Controls and the in-headset menu: see the [README](../README.md#in-headset-menu). Settings live in `gta2ceiling.ini`.

**Rollback:** delete the shortcut (and restore its `.backup`, if one was made) and the mod files; `data/` and `player/` are never changed.
