[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$LoaderAar,
    [string]$BuildRoot = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\roomprobe",
    [string]$AndroidPlayer = 'C:\Program Files\Unity\Hub\Editor\6000.3.10f1\Editor\Data\PlaybackEngines\AndroidPlayer'
)
$ErrorActionPreference = 'Stop'
# Build outputs go to a work root outside the source tree: D:\GTA2Ceiling-work
# by default, or the folder in GTA2CEILING_WORK_ROOT (see docs/BUILDING.md).
function Test-GccWorkPath([string]$Full) {
    if ($env:GTA2CEILING_WORK_ROOT) {
        $root = [IO.Path]::GetFullPath($env:GTA2CEILING_WORK_ROOT).TrimEnd('\') + '\'
        return ($Full.TrimEnd('\') + '\').StartsWith($root, [StringComparison]::OrdinalIgnoreCase)
    }
    return $Full.StartsWith('D:\', [StringComparison]::OrdinalIgnoreCase)
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if (-not (Test-GccWorkPath $BuildRoot)) { throw 'All build output must be under the work root.' }
$ancestor = $BuildRoot
while ($ancestor) {
    if (Test-Path -LiteralPath $ancestor) {
        $item = Get-Item -LiteralPath $ancestor -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Build path crosses a reparse point: $ancestor" }
    }
    $parent = Split-Path -Parent $ancestor
    if ($parent -eq $ancestor) { break }; $ancestor = $parent
}
$expected = '06B92AB3C2D583300ACE5CC84DE7E4E513D5EAE790E2D6D3A1226BC82EE4FF83'
if ((Get-FileHash -LiteralPath $LoaderAar -Algorithm SHA256).Hash -ne $expected) {
    throw 'Loader AAR does not match the reviewed OpenXR 1.1.49 dependency. Review provenance before changing the pin.'
}
$ndk = Join-Path $AndroidPlayer 'NDK'
$sdk = Join-Path $AndroidPlayer 'SDK'
$jdk = Join-Path $AndroidPlayer 'OpenJDK'
$bin = Join-Path $sdk 'build-tools\35.0.0'
$jar = Join-Path $sdk 'platforms\android-34\android.jar'
$clang = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
foreach ($path in @($clang,$jar,"$bin\aapt2.exe","$bin\d8.bat","$bin\zipalign.exe","$bin\apksigner.bat","$jdk\bin\javac.exe","$jdk\bin\keytool.exe")) {
    if (!(Test-Path -LiteralPath $path)) { throw "Missing existing tool: $path" }
}
foreach($leaf in @('temp','logs','deps','classes','dex','lib\arm64-v8a','assets\licenses')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $BuildRoot $leaf) | Out-Null
}
$env:TEMP = Join-Path $BuildRoot 'temp'
$env:TMP = $env:TEMP
$env:JAVA_HOME = $jdk
$env:JAVA_TOOL_OPTIONS = "-Djava.io.tmpdir=$($env:TEMP)"
$env:GRADLE_USER_HOME = Join-Path $BuildRoot 'unused-gradle-cache'

function RunChecked([string]$Name, [string]$Tool, [string[]]$Arguments) {
    $output = & $Tool @Arguments 2>&1
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $BuildRoot "logs\$Name.log") -Encoding utf8
    if ($code -ne 0) { $output | Write-Output; throw "$Name failed ($code)" }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($LoaderAar))
try {
    foreach($entry in $archive.Entries) {
        if (!$entry.Name) { continue }
        if ($entry.FullName.StartsWith('prefab/modules/headers/include/openxr/') -or
            $entry.FullName -eq 'jni/arm64-v8a/libopenxr_loader.so' -or $entry.FullName -eq 'META-INF/LICENSE') {
            $target = [IO.Path]::GetFullPath((Join-Path "$BuildRoot\deps" $entry.FullName))
            if (!$target.StartsWith("$BuildRoot\deps\",[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid AAR member path' }
            New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$true)
        }
    }
} finally { $archive.Dispose() }
Copy-Item -LiteralPath "$BuildRoot\deps\META-INF\LICENSE" -Destination "$BuildRoot\assets\licenses\OpenXR-Loader-Apache-2.0.txt" -Force
Copy-Item -LiteralPath "$BuildRoot\deps\jni\arm64-v8a\libopenxr_loader.so" -Destination "$BuildRoot\lib\arm64-v8a\libopenxr_loader.so" -Force
Copy-Item -LiteralPath "$PSScriptRoot\THIRD_PARTY_NOTICES.md" -Destination "$BuildRoot\assets\licenses\THIRD_PARTY_NOTICES.md" -Force
Copy-Item -LiteralPath "$PSScriptRoot\..\..\LICENSE" -Destination "$BuildRoot\assets\licenses\GTA2Ceiling-LICENSE.txt" -Force

Push-Location $BuildRoot
try {
    RunChecked 'native' $clang @('--target=aarch64-linux-android29','-std=c++17','-O2','-g0','-fPIC','-shared',
        '-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-static-libstdc++',
        '-I',"$BuildRoot\deps\prefab\modules\headers\include", "$PSScriptRoot\roomprobe.cpp",
        '-L',"$BuildRoot\lib\arm64-v8a",'-lopenxr_loader','-lEGL','-lGLESv3','-landroid','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',"$BuildRoot\lib\arm64-v8a\libroomprobe.so")
    RunChecked 'javac' "$jdk\bin\javac.exe" @('-encoding','UTF-8','-source','8','-target','8','-bootclasspath',$jar,
        '-d',"$BuildRoot\classes", "$PSScriptRoot\RoomProbeActivity.java")
    $classes = @(Get-ChildItem -LiteralPath "$BuildRoot\classes" -Recurse -Filter '*.class' | ForEach-Object FullName)
    RunChecked 'd8' "$bin\d8.bat" (@('--min-api','29','--lib',$jar,'--output',"$BuildRoot\dex") + $classes)
    RunChecked 'aapt' "$bin\aapt2.exe" @('link','-I',$jar,'--manifest',"$PSScriptRoot\AndroidManifest.xml",
        '--min-sdk-version','29','--target-sdk-version','32','-A',"$BuildRoot\assets",'-o',"$BuildRoot\unsigned.apk")
    $apk = [IO.Compression.ZipFile]::Open("$BuildRoot\unsigned.apk",[IO.Compression.ZipArchiveMode]::Update)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($apk,"$BuildRoot\dex\classes.dex",'classes.dex') | Out-Null
        foreach($library in @('libroomprobe.so','libopenxr_loader.so')) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($apk,"$BuildRoot\lib\arm64-v8a\$library","lib/arm64-v8a/$library") | Out-Null
        }
    } finally { $apk.Dispose() }
    RunChecked 'zipalign' "$bin\zipalign.exe" @('-f','-p','4',"$BuildRoot\unsigned.apk","$BuildRoot\aligned.apk")
    $key = Join-Path $BuildRoot 'roomprobe-debug.keystore'
    if (!(Test-Path -LiteralPath $key)) {
        RunChecked 'keytool' "$jdk\bin\keytool.exe" @('-genkeypair','-keystore',$key,'-storepass','android',
            '-alias','roomprobe-debug','-keypass','android','-keyalg','RSA','-keysize','2048','-validity','3650',
            '-dname','CN=GTA2Ceiling Room Probe Development','-noprompt')
    }
    RunChecked 'sign' "$bin\apksigner.bat" @('sign','--ks',$key,'--ks-key-alias','roomprobe-debug',
        '--ks-pass','pass:android','--key-pass','pass:android','--out',"$BuildRoot\GTA2CeilingRoomProbe.apk","$BuildRoot\aligned.apk")
    RunChecked 'verify' "$bin\apksigner.bat" @('verify','--verbose','--print-certs',"$BuildRoot\GTA2CeilingRoomProbe.apk")
    RunChecked 'package' "$bin\aapt2.exe" @('dump','badging',"$BuildRoot\GTA2CeilingRoomProbe.apk")
    [ordered]@{
        applicationId='com.gta2ceiling.roomprobe'; loaderAarSha256=$expected;
        loaderHeaderVersion='1.1.49'; ndk=(Get-Content -LiteralPath "$ndk\source.properties" -Raw);
        apkSha256=(Get-FileHash -LiteralPath "$BuildRoot\GTA2CeilingRoomProbe.apk" -Algorithm SHA256).Hash;
        buildUtc=[DateTime]::UtcNow.ToString('o'); configuration='diagnostic-debug'; deviceAcceptance='not yet tested'
    } | ConvertTo-Json | Set-Content -LiteralPath "$BuildRoot\build-manifest.json" -Encoding utf8
    Get-Item -LiteralPath "$BuildRoot\GTA2CeilingRoomProbe.apk" | Select-Object FullName,Length
} finally { Pop-Location }
