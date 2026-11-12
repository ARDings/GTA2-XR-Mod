# Third-Party Notices

Reviewed 8 November 2026. This file records actual source components. It does not assert that external runtimes or unadopted SDKs are bundled in the release. See [DEPENDENCIES.md](DEPENDENCIES.md) and the [source provenance](third_party/gta2-renderer/PROVENANCE.md).

## GTA2 renderer

Source: [gebdag/gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix), commit `537c51192801fb041cd87ac7107103642be3322b`. Original license retained at `third_party/gta2-renderer/LICENSE`. GTA2Ceiling includes an adapted source subset; it does not distribute GTA2 game data or the RTX runtime.

MIT License

Copyright (c) 2026 gebdag

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Separate experimental WinlatorXR host

`runtime/winlatorxr-patches` contains GPL-3.0-or-later host changes for WinlatorXR commit `39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e`. This component is not covered by the mod renderer's MIT license. Its upstream license text is preserved in [COPYING](runtime/winlatorxr-patches/COPYING); build, protocol, donor requirements and corresponding-source scope are documented in [the host README](runtime/winlatorxr-patches/README.md).

Native build inputs are pinned by full file hashes in [dependency-lock.json](runtime/winlatorxr-patches/dependency-lock.json): OpenXR loader/headers 1.1.49 from the specified Unity OpenXR AAR (Apache-2.0), and SDL headers from upstream gitlink `98d1f3a45aae568ccd6ed5fec179330f47d4d356` (Zlib). The NDK r27c static libc++ uses its Apache-2.0/LLVM license notices. The APK also contains unchanged upstream PulseAudio/libsndfile binaries and Gradle-resolved dependencies. Their complete redistribution notices and any required corresponding source must be assembled before a public host APK release; the successful private development build does not claim that release work is complete. No game assets or donor Wine/ImageFS assets are embedded in this APK.

## Dear ImGui

Version `1.91.7 WIP`, as included by the same pinned upstream. The original license is retained at `third_party/gta2-renderer/dll/deps/imgui/LICENSE.txt`.

The MIT License (MIT)

Copyright (c) 2014-2024 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## NVIDIA Remix API headers

The copied API header version is `0.1000.0`. Each original notice remains in its source file:

- `dll/deps/bridge_api/bridge_remix_api.h`: Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
- `dll/deps/bridge_api/remix/remix.h`: Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
- `dll/deps/bridge_api/remix/remix_c.h`: Copyright (c) 2023-2025, NVIDIA CORPORATION. All rights reserved.

The headers carry the following permission and disclaimer. No NVIDIA runtime binary is included by this notice.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

## External runtime and private content

WinlatorXR and its Wine/CPU/graphics components are separately obtained runtime dependencies. Their own notices and distribution obligations remain applicable. If GTA2Ceiling distributes a runtime modification, its corresponding source and licenses must accompany that distribution as required; this file is not a substitute for that obligation.

No MinHook or Khronos OpenXR SDK source is claimed as an adopted dependency in this notice. Such entries must be added only if actual code is incorporated with verified provenance.

Grand Theft Auto 2 and its original game data belong to their respective rights holders. They are supplied privately by users and are not licensed under the project's MIT license.

## JSON for Modern C++

Version v3.12.0, commit 55f93686c01528224f448c19128836e7df245f72. Unmodified single header in third_party/nlohmann-json; https://github.com/nlohmann/json . Used for the private local room-bridge protocol.

MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
