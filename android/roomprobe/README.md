# Quest room diagnostic

Separate diagnostic APK `com.gta2ceiling.roomprobe`. It requests the Android Scene
permission, queries existing ROOM_LAYOUT anchors, loads their CEILING UUIDs, and
reads semantic labels, local plane boundary, pose validity and tracking flags.
It never starts Scene Capture, edits a room, or applies data to GTA2/WinlatorXR.

`files/roomprobe.json` is private app data. A development-device inspection can
read it with `adb shell run-as com.gta2ceiling.roomprobe cat files/roomprobe.json`.
Logcat tag: `GTA2RoomProbe`. Reports distinguish pending/denied permission, missing
extensions, API errors, no saved scene, invalid tracking and successful capture.
Installing or launching the APK alone is not a successful room-data test.

Export schema version 1 uses right-handed meters and `coordinateSpace=NATURAL_STAGE`.
The reference space is created with an identity pose. Polygon coordinates are the
Scene plane's local XY, z=0; `polygon3DStage` applies the returned quaternion/position.
`xrTime` belongs to this OpenXR session. UUIDs identify the room and ceiling; XrSpace
handles are never exported as persistent IDs. Invalid poses produce `planePose:null`
and no stage-space vertices. Room data is private and must not be checked in.

**The export cannot be directly applied to stock WinlatorXR.** That runtime applies
a private yaw/position recenter to its STAGE reference spaces. A separately verified
host bridge must establish the same natural reference frame and handle reference
space changes before GTA2 may consume these polygons. No guessed coordinate offset
is accepted by this probe.

Build without Gradle/downloads using the existing reviewed OpenXR AAR:

```powershell
./build.ps1 -LoaderAar 'PATH_TO_EXISTING_openxr_loader.aar'
```

The script verifies the dependency hash, reads installed tools, and writes native
objects, Java classes, temporary files, logs, APKs and a development key only to
`<work root>\roomprobe` (`GTA2CEILING_WORK_ROOT`, default `D:\GTA2Ceiling-work`). It validates and signs the small ARM64 diagnostic
APK. Permission prompts need a decision in the headset; do not claim granted access
or trustworthy Scene results until the actual report proves them.

API references: [Meta Scene overview](https://developers.meta.com/vr/documentation/native/android/openxr-scene-overview/),
[Scene reference](https://developers.meta.com/vr/documentation/native/android/mobile-scene-api-ref/),
[Spatial data permission](https://developers.meta.com/vr/documentation/native/native-spatial-data-perm/).
