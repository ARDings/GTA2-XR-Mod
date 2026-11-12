# Builds the mod DLLs and the GTA2Ceiling host, then installs both on the
# USB-connected Quest. No game data is copied; only the two mod DLLs and the
# host APK. Build outputs and logs go to the work root: GTA2CEILING_WORK_ROOT,
# or D:\GTA2Ceiling-work by default (see docs/BUILDING.md).
param(
    [string]$WorkRoot = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\implementation",
    [string]$Version = '0.3.4-ceiling-preview',
    [string]$Adb = 'C:\Program Files\Unity\Hub\Editor\6000.3.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\SDK\platform-tools\adb.exe',
    [string]$QuestGameDir = '/sdcard/Download/GTA2Ceiling/gta2',
    [switch]$SkipHost,
    [switch]$SkipMod,
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$status = Join-Path $WorkRoot 'logs\deploy_quest.status'
New-Item -ItemType Directory -Force -Path (Split-Path $status) | Out-Null
'RUNNING' | Set-Content -LiteralPath $status
Start-Transcript -LiteralPath (Join-Path $WorkRoot 'logs\deploy_quest.log') -Force | Out-Null
trap { "FAILED: $_" | Set-Content -LiteralPath $status; Stop-Transcript | Out-Null; break }
$repo = Split-Path -Parent $PSScriptRoot
# Optional machine-specific overrides (not in git): e.g. $GradleCache = 'E:\Cache\Gradle'
$GradleCache = $null
$localSettings = Join-Path $PSScriptRoot 'local.settings.ps1'
if (Test-Path -LiteralPath $localSettings) { . $localSettings }
if (-not (Test-Path -LiteralPath $Adb -PathType Leaf)) { $Adb = (Get-Command adb -ErrorAction Stop).Source }

$devices = & $Adb devices | Select-String -Pattern "`tdevice$"
if (@($devices).Count -ne 1) { throw 'Exactly one authorised Quest must be connected via adb.' }

if (-not $SkipMod) {
    & "$repo\tools\build_mod.ps1" -WorkRoot $WorkRoot -Configuration Release -Version $Version
    $bin = Join-Path $WorkRoot 'build\bin\Release'
    foreach ($dll in 'gta2ceiling.dll', 'gta2ceiling_vid.dll') {
        $local = Join-Path $bin $dll
        if (-not (Test-Path -LiteralPath $local -PathType Leaf)) { throw "Missing build output $local" }
        & $Adb push $local "$QuestGameDir/$dll"
        if ($LASTEXITCODE) { throw "adb push failed for $dll" }
        $want = (Get-FileHash -Algorithm SHA256 -LiteralPath $local).Hash.ToLowerInvariant()
        $have = (& $Adb shell sha256sum "$QuestGameDir/$dll").Split(' ')[0]
        if ($have -ne $want) { throw "Hash mismatch on Quest for $dll" }
        Write-Output "Quest $dll = $want"
    }
    # Sharper output: 3072x1536 (1536 px per eye). Update the Quest's game ini
    # in place; other personal settings in it stay untouched.
    $ini = "$QuestGameDir/gta2ceiling.ini"
    & $Adb shell "sed -i -e 's/^render_width=.*/render_width=3072/' -e 's/^render_height=.*/render_height=1536/' $ini"
    if ($LASTEXITCODE) { throw 'Could not update render size in the Quest gta2ceiling.ini' }
}

if (-not $SkipHost) {
    $hostArgs = @{ WorkRoot = "$WorkRoot\winlator-host"; Offline = $true }
    if ($GradleCache) { $hostArgs.GradleCache = $GradleCache }
    & "$repo\runtime\winlatorxr-patches\BuildHost.ps1" @hostArgs
    $apk = "$WorkRoot\winlator-host\fork\app\build\outputs\apk\debug\app-debug.apk"
    # -r keeps the host's private ImageFS, containers and room permission.
    & $Adb install -r $apk
    if ($LASTEXITCODE) { throw 'Host APK install failed' }
}
$package = 'com.gta2ceiling.winlator'
& $Adb shell am force-stop $package
if (-not $NoLaunch) {
    # Find the container that holds the GTA2Ceiling shortcut (debug build: run-as works).
    $homes = & $Adb shell run-as $package ls files/imagefs/home
    $shortcut = $null; $container = $null
    foreach ($h in $homes) {
        $h = $h.Trim()
        if ($h -notmatch '^xuser-(\d+)$') { continue }
        $candidate = "/data/data/$package/files/imagefs/home/$h/.wine/drive_c/users/xuser/Desktop/GTA2Ceiling.desktop"
        & $Adb shell run-as $package test -f $candidate
        if ($LASTEXITCODE -eq 0) { $shortcut = $candidate; $container = [int]$Matches[1]; break }
    }
    if (-not $shortcut) { throw 'GTA2Ceiling shortcut not found in any host container.' }
    # The X screen (and so the SBS image the host shows) follows the shortcut.
    & $Adb shell run-as $package sed -i 's/^screenSize=.*/screenSize=3072x1536/' $shortcut
    if ($LASTEXITCODE) { throw 'Could not update the shortcut screen size' }
    # A whole session of host logs (the Quest system logs a lot); then a fresh log.
    try { & $Adb logcat -G 16M 2>$null | Out-Null } catch { }
    try { & $Adb logcat -c 2>$null | Out-Null } catch { }
    & $Adb shell am start -n "$package/com.winlator.xr.runtime.MetaQuest" --ei container_id $container --es shortcut_path $shortcut
    if ($LASTEXITCODE) { throw 'Launch failed' }
    Write-Output "Started GTA2Ceiling (container $container) on the Quest."
}
Write-Output 'Deploy complete.'
'OK' | Set-Content -LiteralPath $status
Stop-Transcript | Out-Null
