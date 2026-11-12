param(
    [string]$WorkRoot = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\build-current",
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateRange(1,16)][int]$ParallelJobs = 2,
    [string]$Version = '0.2.0-device-test',
    [string]$CMake = 'C:\VS\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
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
$sourceRoot = Split-Path -Parent $PSScriptRoot
$taskWork = [IO.Path]::GetFullPath($WorkRoot)
if (-not (Test-GccWorkPath $taskWork)) {
    throw 'Builds, caches and logs must be under the work root (D:\GTA2Ceiling-work or GTA2CEILING_WORK_ROOT).'
}
if (-not (Test-Path -LiteralPath $CMake -PathType Leaf)) {
    $CMake = (Get-Command cmake -ErrorAction Stop).Source
}
$testRunner = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
$build = Join-Path $taskWork 'build'
$logs = Join-Path $taskWork 'logs'
$temporary = Join-Path $taskWork 'temp'
foreach ($target in @($taskWork,$build,$logs,$temporary,(Join-Path $taskWork 'release'))) {
    $ancestor=$target
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item=Get-Item -LiteralPath $ancestor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Build output cannot traverse a junction or symbolic link: $ancestor"
            }
        }
        $ancestor=Split-Path -Parent $ancestor
    }
}
New-Item -ItemType Directory -Force -Path $logs,$temporary | Out-Null
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$previousBytecode = $env:PYTHONDONTWRITEBYTECODE
function Run-Checked([string]$Program, [string[]]$Arguments, [string]$LogName) {
    # Windows PowerShell 5.1 turns redirected native stderr (unittest progress)
    # into terminating errors under Stop; the exit code below is authoritative.
    $ErrorActionPreference = 'Continue'
    & $Program @Arguments *> (Join-Path $logs $LogName)
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath (Join-Path $logs $LogName) -Tail 35
        throw "$Program failed with exit code $LASTEXITCODE; see $logs\$LogName"
    }
}
try {
    $env:TEMP = $temporary
    $env:TMP = $temporary
    $env:PYTHONDONTWRITEBYTECODE = '1'
    Run-Checked $CMake @('-S',$sourceRoot,'-B',$build,'-G','Visual Studio 17 2022','-A','Win32') 'configure.log'
    Run-Checked $CMake @('--build',$build,'--config',$Configuration,'--parallel',"$ParallelJobs") 'build.log'
    Run-Checked $testRunner @('--test-dir',$build,'-C',$Configuration,'--output-on-failure','--timeout','30') 'ctest.log'
    Run-Checked 'python' @('-B','-m','unittest','discover','-s',(Join-Path $sourceRoot 'tests'),'-p','test_*.py','-v') 'python-tests.log'
    Run-Checked 'python' @('-B',(Join-Path $sourceRoot 'tools\build_release_package.py'),'--build-bin',(Join-Path $build "bin\$Configuration"),'--output',(Join-Path $taskWork 'release'),'--version',$Version) 'package.json'
    Get-Content -LiteralPath (Join-Path $logs 'package.json')
    Write-Output 'Build and local checks complete. Quest gameplay acceptance remains a separate device test.'
}
finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    $env:PYTHONDONTWRITEBYTECODE = $previousBytecode
}
