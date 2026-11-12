# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$SourceRoot = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\implementation\winlator-host\fork",
    [string]$OutputRoot = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\implementation\winlator-host\native-build",
    [string]$NdkRoot = 'C:\Program Files\Unity\Hub\Editor\6000.3.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK',
    [string]$OpenXrInclude = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\roomprobe\deps\prefab\modules\headers\include",
    [string]$OpenXrLoader = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\roomprobe\deps\jni\arm64-v8a\libopenxr_loader.so",
    [string]$SdlInclude = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\implementation\winlator-host\native-build\deps",
    [ValidateSet('xr','winlator','evshim','kgsl_turnip_shim','map_merge_shim','turnip_compat_shim')]
    [string[]]$Targets = @('xr','winlator','evshim','kgsl_turnip_shim','map_merge_shim','turnip_compat_shim')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Build outputs go to a work root outside the source tree: D:\GTA2Ceiling-work
# by default, or the folder in GTA2CEILING_WORK_ROOT (see docs/BUILDING.md).
function Test-GccWorkPath([string]$Full) {
    if ($env:GTA2CEILING_WORK_ROOT) {
        $root = [IO.Path]::GetFullPath($env:GTA2CEILING_WORK_ROOT).TrimEnd('\') + '\'
        return ($Full.TrimEnd('\') + '\').StartsWith($root, [StringComparison]::OrdinalIgnoreCase)
    }
    return $Full.StartsWith('D:\', [StringComparison]::OrdinalIgnoreCase)
}
function AssertDPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-GccWorkPath $full)) { throw "Build output must be under the work root: $full" }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Build path cannot traverse a reparse point: $cursor" }
        }
        $parent = [IO.Directory]::GetParent($cursor)
        $cursor = if ($parent) { $parent.FullName } else { $null }
    }
    return $full
}
$OutputRoot = AssertDPath $OutputRoot
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$cpp = Join-Path $SourceRoot 'app\src\main\cpp'
$bin = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$clang = Join-Path $bin 'clang.exe'; $clangxx = Join-Path $bin 'clang++.exe'
$readelf = Join-Path $bin 'llvm-readelf.exe'
foreach ($required in @($clang,$clangxx,$readelf,$OpenXrLoader,(Join-Path $OpenXrInclude 'openxr\openxr.h'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing reviewed local dependency: $required" }
}
$temp = Join-Path $OutputRoot 'temp'; $objects = Join-Path $OutputRoot 'obj'
$logs = Join-Path $OutputRoot 'logs'; $libs = Join-Path $OutputRoot 'jniLibs\arm64-v8a'
foreach ($directory in @($temp,$objects,$logs,$libs)) {
    $null = AssertDPath $directory
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$env:TEMP = $temp; $env:TMP = $temp; $env:TMPDIR = $temp
$env:CCACHE_DIR = Join-Path $OutputRoot 'ccache'
function RunChecked([string]$Exe,[string[]]$Arguments,[string]$LogName) {
    # PowerShell 5.1: compiler warnings on stderr must not become terminating errors.
    $ErrorActionPreference = 'Continue'
    $output = @(& $Exe @Arguments 2>&1)
    $code = $LASTEXITCODE
    [string]::Join([Environment]::NewLine, @($output | ForEach-Object { "$_" })) | Set-Content -LiteralPath (Join-Path $logs $LogName) -Encoding utf8
    if ($code -ne 0) { throw "Native tool failed ($code): $Exe; see $LogName" }
}
$common = @('--target=aarch64-linux-android29','-fPIC','-O2','-ffunction-sections','-fdata-sections',"-I$OpenXrInclude", "-I$OpenXrInclude\openxr")
$records = @()
Push-Location $OutputRoot
try {
    foreach ($target in $Targets) {
        $sources = @(); $libraries = @(); $includes = @()
        if ($target -eq 'xr') {
            if (-not (Select-String -LiteralPath (Join-Path $cpp 'xr\main.cpp') -SimpleMatch 'GccRoomBridge' -Quiet)) { throw 'The supplied fork is not patched with the RoomBridge native hooks.' }
            $sources = @('engine.c','edgeglow.c','framebuffer.c','input.c','direct.c','main.cpp','math.c','oneeuro.c','renderer.c','gcc_room_bridge.cpp','gcc_scene_query.cpp','gcc_occlusion.cpp','gcc_reproject.cpp') | ForEach-Object { Join-Path $cpp "xr\$_" }
            $libraries = @($OpenXrLoader,'-llog','-lEGL','-lGLESv3','-lnativewindow','-ljnigraphics','-landroid','-ldl')
        } elseif ($target -eq 'winlator') {
            $sources = @('drawable.c','gpu_image.c','sysvshared_memory.c','xconnector_epoll.c','alsa_client.c','patchelf_wrapper.cpp','vulkan.cpp') | ForEach-Object { Join-Path $cpp "winlator\$_" }
            $libraries = @('-llog','-landroid','-ljnigraphics','-laaudio','-lEGL','-lGLESv2','-ldl')
        } else {
            $sources = @(Join-Path $cpp "winlator\$target.c")
            $libraries = @('-llog','-ldl')
            if ($target -eq 'evshim') {
                if (-not (Test-Path -LiteralPath (Join-Path $SdlInclude 'SDL2\SDL.h'))) { throw 'SDL headers required for evshim; use exact Winlator gitlink and preserve LICENSE.' }
                $includes += "-I$SdlInclude"
                # Upstream evshim resolves SDL at runtime via dlopen/dlsym. No
                # surrogate SDL library or invented ABI declarations are linked.
            }
        }
        $targetObjects = @()
        foreach ($source in $sources) {
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing source: $source" }
            $leaf = [IO.Path]::GetFileNameWithoutExtension($source)
            $object = Join-Path $objects "$target-$leaf.o"
            $isCpp = [IO.Path]::GetExtension($source) -eq '.cpp'
            $compiler = if ($isCpp) { $clangxx } else { $clang }
            $language = if ($isCpp) { @('-std=c++17') } else { @('-std=gnu11','-Wno-error=implicit-function-declaration') }
            RunChecked $compiler ($common + $language + $includes + @('-c',$source,'-o',$object)) "$target-$leaf.log"
            $targetObjects += $object
        }
        $library = Join-Path $libs "lib$target.so"
        RunChecked $clangxx (@('--target=aarch64-linux-android29','-shared','-static-libstdc++','-Wl,--exclude-libs,ALL','-Wl,--gc-sections','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',"-Wl,-soname,lib$target.so") + $targetObjects + $libraries + @('-o',$library)) "$target-link.log"
        RunChecked $readelf @('-h','-d',$library) "$target-elf.txt"
        $records += @{ target=$target; path=$library; sha256=(Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash; bytes=(Get-Item -LiteralPath $library).Length }
        Write-Output "Built lib$target.so"
    }
    Copy-Item -LiteralPath $OpenXrLoader -Destination (Join-Path $libs 'libopenxr_loader.so') -Force
    @{ buildUtc=[DateTime]::UtcNow.ToString('o'); source=$SourceRoot; abi='arm64-v8a'; minApi=29; targets=$records;
       loaderSha256=(Get-FileHash -LiteralPath $OpenXrLoader -Algorithm SHA256).Hash;
       note='Native compile/link evidence only. Requires complete host APK and Quest runtime acceptance. libc++ is statically linked with hidden archive symbols.'
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputRoot 'native-build-manifest.json') -Encoding utf8
} finally { Pop-Location }
