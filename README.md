# GTA2Ceiling

**Grand Theft Auto 2 as a living miniature city on your real ceiling or floor, in mixed reality on a standalone Meta Quest 3.**

The rest of the room stays visible through passthrough. You lean back, look up, and the original 1999 city runs above you: real building heights, two eye views, head parallax, cars and people with a little body — driven by the original game simulation and an Xbox controller connected to the Quest, or just the Quest's own Touch controllers. No PC is needed while playing.

> **Status: experimental preview.** It runs on a real Quest 3 and is playable, but it is a hobby research project, not a finished release. Expect rough edges. This repository is source only: there is no ready-made APK or mod download yet, you build both yourself ([docs/BUILDING.md](docs/BUILDING.md)). Deutsche Kurzfassung: [README.de.md](README.de.md).

> **You need your own copy of GTA2.** This repository contains no game files, textures, maps, audio, video or executables from GTA2, and none are ever downloaded or redistributed. Grand Theft Auto and GTA2 are trademarks of their respective owners; this project is not affiliated with or endorsed by them.

## What it does

- **City on the real room surface** – the Quest's own room scan (Scene API) defines the ceiling or floor outline; the city is masked to it and buildings hanging near a wall stay visible in front of the wall.
- **Play-field modes** – the whole ceiling/floor or a circle on it (0.5 to 3 m, adjustable in the headset).
- **Locked to the room at display rate** – the game renders one stereo pair per simulation frame (30/s); on every display frame (72 Hz, or 60 Hz where offered) the host re-renders that image for the current head pose by intersecting each pixel ray with the scanned ceiling/floor plane, so head rotation *and* head movement are corrected however old the game image is.
- **Real-world occlusion** – your hands, people and furniture in front of the city hide it, using the Quest 3 depth sensor against the scanned room shell.
- **Readable miniature** – cars and pedestrians become small stacked-sprite bodies.
- **Fixed, sharp image** – the scene renders 1:1 at the window size with anisotropic filtering, shown 1:1 in the host eye buffers (no MSAA and no shadows: the GPU budget goes to a steady frame rate); occlusion can be toggled in the headset.
- **Clean passthrough edge** – the city is cut out against the room with an exact colour key and its own alpha coverage, so there are no pink fringes.
- **Walk out of the game** – freeze the city on a round field and drive or walk off its edge onto your real floor, then back in anywhere.
- **Optional holo cut** – buildings cut open by the edge of the round field are closed with a holographic surface.
- **Controller-first** – Xbox controller (or the Quest Touch controllers in the same layout) with GTA2's original controls or an optional modern scheme (stick direction on foot, triggers in a car); a hidden in-headset menu for placement and options.

### In-headset menu

Press **both sticks** briefly to open it (hold them for 1 s to put your character directly above/below you).

In the circle, **hold the right stick for 4 s** to freeze the city in place while the game goes on. Walk or drive off the edge and your character (or your car) leaves the disc onto your real floor at full speed, with the game's own controls and physics: only the player is shown outside, and outside the disc there are no walls or streets to run into, because while the city is frozen the game's map out there is flat ground (in memory only, put back exactly as it was afterwards). Drive around the outside and back into the disc anywhere: the map shows the same section the whole time and the game simply goes on at the spot where you come back in. Hold the right stick again for 4 s to let the city follow the player again; nothing jumps, the city glides back under the player within a second and a half.

| Button | Function |
|---|---|
| Left stick / right stick | Move / rotate the city or play field |
| LB / RB | City scale |
| Back | Ceiling ↔ floor |
| D-pad left/right | Whole surface / circle |
| D-pad up/down | Circle size (0.5 to 3 m) |
| Y | View range: how far from the player cars and people appear (1x to 3x) |
| RT | Controls: classic (GTA2's own: left/right turn, up walks) or modern (see below) |
| X | Holo cut (round field): buildings cut open by the edge of the field are closed with a holographic surface on the cut, so you cannot look inside (off by default) |
| Start | Real-world occlusion |
| A / B | Save / discard |

### Quest Touch controllers

No Xbox controller? When none is connected, the GTA2Ceiling host turns the two Touch controllers into an Xbox pad with the same layout, so everything above works the same. Connect an Xbox controller and it takes over at once.

| Xbox | Touch |
|---|---|
| Left / right stick, L3 / R3 | Left / right thumbstick, pressed |
| A / B, X / Y | A / B (right), X / Y (left) |
| LB / RB | Left / right grip |
| LT / RT | Left / right trigger |
| Start | ≡ button, short press |
| Back | ≡ button, held |
| D-pad | Hold both grips, then the left stick |

The WinlatorXR system menu opens when you hold the **left** thumbstick alone for a second (the right one belongs to the game).

### Modern controls (optional)

GTA2 steers like a tank: left/right turn the character, up walks where it faces. With **RT** in the menu you can switch to a modern scheme. Nothing in the game is changed; the mod only holds different keys for you.

- **On foot:** the left stick points where the character should go, as you see the city from where you stand. It turns that way by itself and walks.
- **In a car:** RT accelerates, LT brakes and reverses, the left stick steers, B fires. A still gets in and out.
- Menus, the D-pad and all other buttons stay as they are. Until the mod has found the player in the game's memory (a few seconds of moving around on the very first start, remembered afterwards), the pad behaves classically.

## How it works

```
Quest 3 (standalone)
└─ GTA2Ceiling host  ── fork of WinlatorXR (GPL-3.0), runtime/winlatorxr-patches
   ├─ OpenXR session: passthrough, Scene API (ceiling/floor), environment depth
   ├─ GCCROOM1: authenticated loopback channel that hands the room scan to the game
   ├─ exact-colour-key compositing + display-rate planar reprojection + occlusion pass
   └─ Wine/Box64 container
      └─ original gta2.exe (your copy) + gta2ceiling.dll (this repo, MIT)
         ├─ takes over GTA2's renderer (D3D9 via DXVK): real 3D city mesh from the map
         ├─ XrAPI tracking → per-eye stereo pairs on every new head pose
         └─ room-surface mask, play fields, sprite bodies, menu
```

The renderer started from the MIT-licensed [gebdag/gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix) (pinned, see [PROVENANCE](third_party/gta2-renderer/PROVENANCE.md)); RTX Remix itself is not used. Key GTA2Ceiling changes:

- a stereo XR path that renders the city on head-pose refreshes between simulation ticks, with a reserved sync pixel that tells the host which pose each frame belongs to;
- an alpha-coverage scene target that turns MSAA coverage into an exact passthrough key after resolve;
- the room-surface/wall-band stencil mask, round play fields and a fixed street level;
- batched world-space sprite bodies;
- a host-side fork of WinlatorXR that adds Scene API access, environment-depth occlusion, exact-key filtering and robust frame-sync decoding.

## Building

Builds currently target **Windows** (MSVC 2022, CMake) for the x86 mod DLLs and the **Android NDK/Gradle** shipped with Unity for the host APK. All build outputs go to a work root outside the source tree: `D:\GTA2Ceiling-work` by default, or the folder in the `GTA2CEILING_WORK_ROOT` environment variable. Details: [docs/BUILDING.md](docs/BUILDING.md).

- Mod only: `tools\build_mod.ps1` – builds, runs the C++ and Python tests and creates a mod package without any game data.
- Host APK: see [runtime/winlatorxr-patches/README.md](runtime/winlatorxr-patches/README.md) (needs a pinned WinlatorXR checkout and an OpenXR loader AAR).
- Everything onto a USB-connected Quest: `tools\deploy_quest.cmd`; logs back from the headset: `tools\collect_quest_logs.cmd`. Machine-specific paths go into `tools\local.settings.ps1` (not committed).

Installing on the Quest with your own GTA2 copy: [docs/INSTALL.md](docs/INSTALL.md).

## Author

Built by **Christoph Spinger**. Questions, feedback, or a clip of the city on your own ceiling: [LinkedIn](https://www.linkedin.com/in/christoph-spinger-280621190/).

## Licenses

- GTA2Ceiling mod and tools: [MIT](LICENSE).
- Host changes in `runtime/winlatorxr-patches`: GPL-3.0-or-later, like WinlatorXR ([COPYING](runtime/winlatorxr-patches/COPYING)). A distributed host APK must come with its corresponding source.
- Third-party code and notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [DEPENDENCIES.md](DEPENDENCIES.md).

## Credits

- Renderer base: [gebdag/gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix) (MIT).
- Host: [WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) (GPL-3.0).
- The walk-out idea (leaving the frozen city onto the real floor) was inspired by Lucas.

Contributions, bug reports and headset recordings are welcome.
