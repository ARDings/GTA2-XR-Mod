# Dependencies and provenance

As of November 2026. This lists what is actually in the source tree or used by the builds. Nothing here is a statement that a combination was tested beyond the author's own Quest 3.

## Source included in this repository

| Component | Origin / pin | License and use |
|---|---|---|
| GTA2 renderer | [gebdag/gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix), commit `537c51192801fb041cd87ac7107103642be3322b` | MIT; adapted source subset in `third_party/gta2-renderer` |
| Dear ImGui | part of the pinned upstream, `1.91.7 WIP` | MIT; license at `third_party/gta2-renderer/dll/deps/imgui/LICENSE.txt` |
| NVIDIA Remix API headers | part of the pinned upstream, API `0.1000.0` | MIT notices in the headers; no RTX runtime is shipped or used |
| JSON for Modern C++ | v3.12.0, commit `55f93686c01528224f448c19128836e7df245f72`, unchanged single header in `third_party/nlohmann-json` | MIT |

The exact paths taken over from the renderer upstream are in [PROVENANCE.md](third_party/gta2-renderer/PROVENANCE.md). Local changes are GTA2Ceiling's own.

## The Quest host (separate app)

`runtime/winlatorxr-patches` is a set of GPL-3.0-or-later changes for [WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) at commit `39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e`. The build produces a separate Android app, `com.gta2ceiling.winlator`, with its own app id and private data. It is not covered by the mod's MIT license.

Used by that build and pinned by file hashes in [dependency-lock.json](runtime/winlatorxr-patches/dependency-lock.json):

- OpenXR loader and headers 1.1.49 from Unity's `com.unity.xr.openxr@b5b77b4027be` AAR (Apache-2.0)
- SDL headers at gitlink `98d1f3a45aae568ccd6ed5fec179330f47d4d356` (Zlib)
- Android NDK 27.2.12479018 (static libc++: Apache-2.0 with LLVM exception), AGP 8.10.0, Gradle 8.13, Kotlin plugin 2.2.20

The host contains no Wine/ImageFS assets. At first start it reads them from the official WinlatorXR Dawn42 app that must be installed next to it (checked by version code 24, version name `dawn-42` and signing certificate). Wine, Box64, DXVK and the other runtime parts keep their own licenses.

Anyone who distributes a built host APK must provide its complete corresponding source and the notices of everything inside it; see the [host README](runtime/winlatorxr-patches/README.md).

## Not used

No MinHook, no OpenXR SDK sources and no code from `gta2_re` or OpenGTA2 are part of this project.

## Game data

You bring your own GTA2 (version 9.6). Its executable, DLLs, maps, textures, scripts, audio, video and saves, and anything generated from them, stay private: they are not in this repository and must not be put into a release or a pull request.
