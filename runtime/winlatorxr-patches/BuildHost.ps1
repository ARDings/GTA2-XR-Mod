# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [string]$WorkRoot = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\implementation\winlator-host",
    [string]$GradleCache = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\cache\gradle",
    [string]$AndroidPlayer = 'C:\Program Files\Unity\Hub\Editor\6000.3.10f1\Editor\Data\PlaybackEngines\AndroidPlayer',
    [switch]$Offline
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
foreach ($target in @($WorkRoot, $GradleCache)) {
    $resolved = [IO.Path]::GetFullPath($target)
    if (-not (Test-GccWorkPath $resolved)) { throw 'All build paths must be under the work root' }
    $ancestor = $resolved
    while ($ancestor) {
        if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse build path rejected: $ancestor" }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
}
$fork = Join-Path $WorkRoot 'fork'
New-Item -ItemType Directory -Force -Path "$WorkRoot\tmp", "$WorkRoot\home", "$WorkRoot\logs" | Out-Null
$env:TEMP = "$WorkRoot\tmp"
$env:TMP = $env:TEMP
$env:GRADLE_USER_HOME = $GradleCache
$env:ANDROID_USER_HOME = "$WorkRoot\home\.android"
$env:JAVA_HOME = "$AndroidPlayer\OpenJDK"
$env:ANDROID_HOME = "$AndroidPlayer\SDK"
$env:KOTLIN_DAEMON_RUN_FILES_PATH = "$WorkRoot\tmp\kotlin"
& python -X utf8 -B "$PSScriptRoot\apply_host_patch.py" $fork
if ($LASTEXITCODE) { throw 'Pinned source patch failed' }
& python -X utf8 -B "$PSScriptRoot\prepare_dependencies.py" --verify-only --offline
if ($LASTEXITCODE) { throw 'Native dependencies do not match dependency-lock.json' }
& "$PSScriptRoot\BuildNative.ps1" -SourceRoot $fork -OutputRoot "$WorkRoot\native-build" -NdkRoot "$AndroidPlayer\NDK"
# The native script throws on failure. Never package an older libxr after a source change.
"sdk.dir=$($env:ANDROID_HOME.Replace('\','/'))" | Set-Content -LiteralPath "$fork\local.properties" -Encoding utf8
$java = "$env:JAVA_HOME\bin\java.exe"
if (-not (Test-Path -LiteralPath "$WorkRoot\gcc-debug.keystore")) {
    & "$env:JAVA_HOME\bin\keytool.exe" "-J-Djava.io.tmpdir=$env:TEMP" "-J-Duser.home=$WorkRoot\home" -genkeypair -keystore "$WorkRoot\gcc-debug.keystore" -storepass android -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=GTA2Ceiling Development,O=GTA2Ceiling,C=DE'
    if ($LASTEXITCODE) { throw 'Development signing key generation failed' }
}
$launcher = Get-ChildItem -LiteralPath "$AndroidPlayer\Tools\gradle\lib" -Filter 'gradle-launcher-*.jar' | Select-Object -First 1
$arguments = @("-Djava.io.tmpdir=$env:TEMP", "-Duser.home=$WorkRoot\home", '-classpath', $launcher.FullName, 'org.gradle.launcher.GradleMain', '-p', $fork, '--no-daemon', '--console=plain', '--stacktrace', '-Dorg.gradle.java.installations.auto-download=false', "-Dorg.gradle.jvmargs=-Xmx3072m -Dfile.encoding=UTF-8 -Djava.io.tmpdir=$env:TEMP -Duser.home=$WorkRoot\home", ':app:assembleDebug')
if ($Offline) { $arguments += '--offline' }
# PowerShell 5.1 would turn Gradle's stderr into a terminating error under Stop.
$ErrorActionPreference = 'Continue'
& $java @arguments 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath "$WorkRoot\logs\gradle-host-build.log"
$gradleExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($gradleExit) { throw "Host build failed; see $WorkRoot\logs\gradle-host-build.log" }
Get-FileHash -Algorithm SHA256 -LiteralPath "$fork\app\build\outputs\apk\debug\app-debug.apk"
