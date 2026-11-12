# GTA2Ceiling host (WinlatorXR changes)

This directory turns a pinned [WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) checkout into the GTA2Ceiling host: a separate Android app for the Quest 3 that runs the game in Wine/Box64 and adds what the ceiling city needs, in the **same OpenXR session** the game's head poses come from:

- the Quest's room scan (Scene API) handed to the game,
- display-rate reprojection of the game image onto the scanned ceiling/floor plane,
- real-world occlusion from the depth sensor,
- an exact colour key for passthrough.

Upstream pin: commit `39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e`. App id: `com.gta2ceiling.winlator`; it does not replace or modify the original `com.winlator.cmod`. Java namespace and JNI names stay `com.winlator.cmod` / `com.winlator.xr` for compatibility. This is development source, not a reviewed release.

## Thin package

The host APK contains no Wine/ImageFS assets and no GTA2 files. `GccAssets.java` reads the public APK assets of the installed official WinlatorXR Dawn42 app, after checking package `com.winlator.cmod`, version code `24`, version name `dawn-42` and the signing certificate SHA-256 `2559de0328c65536a42906a31697514b508dbd72dfa030a313b31a444cfe767e`. Any other or missing donor is refused, so keep the original app installed.

At first start the host unpacks those assets into its **own** private ImageFS. Containers, settings and saves of the original app are neither read nor changed. The Quest needs free storage for this: a thin APK is not a small install.

## Building

Windows, PowerShell. You need the Android NDK r27c, SDK and Gradle 8.13 as shipped with Unity 6000.3 (paths are script parameters), Python 3.12+ and git. All outputs, temporary files, the debug signing key and caches go to the work root (`GTA2CEILING_WORK_ROOT`, default `D:\GTA2Ceiling-work`, see [docs/BUILDING.md](../../docs/BUILDING.md)).

```powershell
# Once: a checkout of the pinned commit without the large runtime assets.
$fork = "$env:GTA2CEILING_WORK_ROOT\implementation\winlator-host\fork"   # or D:\GTA2Ceiling-work\...
git clone --filter=blob:none --no-checkout https://github.com/WinlatorXR/WinlatorXR.git $fork
git -C $fork sparse-checkout set --no-cone '/*' '!/app/src/main/assets/'
git -C $fork checkout 39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e
# OpenXR loader from an existing Unity project (read only); SDL headers are fetched small and pinned.
python -X utf8 -B runtime/winlatorxr-patches/prepare_dependencies.py --loader-aar '<UnityProject>\Library\PackageCache\com.unity.xr.openxr@b5b77b4027be\RuntimeLoaders\android\openxr_loader.aar'
# Patch, native targets and APK; add -Offline for later runs.
.\runtime\winlatorxr-patches\BuildHost.ps1
```

- `apply_host_patch.py` only accepts that exact commit in a checkout under the work root. It checks the original contents and the hashes of its own earlier output, never overwrites unknown local changes, and records its state in `.gcc-patch-state.json`, so it can be re-applied.
- `dependency-lock.json` pins the loader AAR, the nine OpenXR files used and 92 SDL files including the license. Another loader is not accepted silently. No full OpenXR/SDL checkout and no ImageFS is downloaded.
- The first Gradle build may download Maven dependencies into the given cache. The debug key it creates is a development artifact, not a release key.

## Room channel GCCROOM1

The game asks the host for the room over UDP on `127.0.0.1:7279` (requests up to 512 bytes, JSON answers up to 60,000 bytes, at most eight requests per XR frame). The host answers on the XR thread; a running scene query never blocks an answer.

```text
GCCROOM1 CAPS <nonce>
GCCROOM1 GET_ROOM <nonce> <baseIndex> <sessionId> <referenceEpoch> <token>
GCCROOM1 RAW_STAGE <nonce> <baseIndex> <sessionId> <referenceEpoch> <token>
GCCROOM1 REFRESH <nonce> <sessionId> <referenceEpoch> <token>
```

- `CAPS` reports protocol, upstream commit, session id, reference epoch, HMD sync and recenter counters, `XrTime`, capabilities (`rawStage`, `sceneQuery`), permission state and `tokenReady`.
- Everything except `CAPS` needs a random 32-digit hex token. The host writes it with mode 0600 to `imagefs/tmp/xr/gta2ceiling_room.token` (`Z:\tmp\xr\gta2ceiling_room.token` inside Wine). It is never logged or passed on a command line.
- `GET_ROOM` locates every ceiling/floor plane freshly in the existing XrAPI base space `baseIndex` (GTA2 uses 1). The answer is in metres, right-handed OpenXR coordinates, with `polygon2D`, `planePose`, `polygon3DBase`, UUIDs, labels and pose flags. Only `scene.status == SUCCESS_CEILING_DATA` with valid pose flags may be used. Pending queries, missing permission, an unknown base, invalid poses, timeouts and runtime errors never produce made-up geometry.
- `RAW_STAGE` locates an unmodified natural STAGE space in the chosen base. It does not change the meaning of the stock XrAPI space type 3, which Winlator recentres internally.
- A new session, a recenter, an OpenXR reference-space change or a host base change invalidates the epoch: the client must drop its geometry and ask again. The room answer and the pose packets switch to a new reference id together, in the same frame.

## Permissions and display

The app declares `com.oculus.permission.USE_ANCHOR_API` and asks for `com.oculus.permission.USE_SCENE` only when a room is actually requested. The existing room scan is read; no new scan is started.

Only a shortcut with `gcc_exact_colour_key=1` switches on the exact key: 8-bit magenta `(255,0,255)` alone becomes transparent, every other colour stays opaque. Winlator's usual wide HSV key and its modes 0 to 4 are unchanged.

Without a physical game controller, the Touch controllers drive a virtual Xbox pad on player 1 (WinlatorXR's gamepad emulation, switched on and off automatically about once a second; mapped keys and mouse emulation are off meanwhile). A connected controller always takes over. WinlatorXR's own menu opens with the left thumbstick held alone, because the right stick press belongs to the game.

If player 1 is enabled and unassigned, the host assigns exactly one detected Microsoft Xbox controller. With several matching devices, player 1 switched off or an existing assignment, nothing is changed.

## License and distributing an APK

The changes in this directory are GPL-3.0-or-later, like WinlatorXR ([COPYING](COPYING)). They are a separate host component; the MIT license of the mod does not apply to them.

If you distribute a built host APK, you must offer its complete corresponding source with it: the exact upstream tree, this whole directory, `dependency-lock.json`, your build scripts, and the license and source notices of everything linked or bundled (OpenXR loader: Apache-2.0; SDL headers: Zlib; static libc++ from the NDK: Apache-2.0 with LLVM exception; upstream's PulseAudio/libsndfile binaries and Java/AAR dependencies under their own terms). `git -C $fork diff --binary 39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e` exports the applied changes, but a diff alone misses the new overlay and Java files. Never publish keystores, private tokens or game data. The build is reproducible from commit plus patch; a bit-identical APK is not promised (signing key, build metadata).
