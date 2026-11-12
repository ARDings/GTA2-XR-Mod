# Room probe dependencies

The app's own source follows the GTA2Ceiling repository license. It contains no
GTA2 executable, original game data, Unity engine, or WinlatorXR code.

The Android build uses Khronos OpenXR loader/header artifacts from an existing
Unity OpenXR package cache. The reviewed `openxr_loader.aar` SHA-256 is
`06b92ab3c2d583300ace5cc84de7e4e513d5eae790e2d6d3a1226bc82ee4ff83`.
Its headers identify OpenXR 1.1.49. Only the ARM64 loader, OpenXR headers, and
the AAR's `META-INF/LICENSE` are extracted into the D: build directory. The
Apache-2.0 license is included under APK assets/licenses. No Unity code is copied.

Upstream: https://github.com/KhronosGroup/OpenXR-SDK
License: https://github.com/KhronosGroup/OpenXR-SDK/blob/main/LICENSE
The existing NDK, Android SDK tools and JDK are build tools and are not shipped.

This development APK uses its own local debug certificate and application ID
`com.gta2ceiling.roomprobe`. It must not replace another application.
