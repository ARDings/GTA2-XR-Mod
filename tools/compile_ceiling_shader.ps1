param(
    [string]$OutputRoot = "$(if ($env:GTA2CEILING_WORK_ROOT) { $env:GTA2CEILING_WORK_ROOT } else { 'D:\GTA2Ceiling-work' })\shader-build",
    [string]$FxcPath = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'
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
$artifactRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-GccWorkPath $artifactRoot)) {
    throw 'Shader artifacts, logs, and temporary files must stay under the work root.'
}
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourcePath = Join-Path $repoRoot 'src\mod\ceiling_present.hlsl'
$headerPath = Join-Path $repoRoot 'src\mod\ceiling_present_shader.h'
if (!(Test-Path -LiteralPath $FxcPath -PathType Leaf)) { throw 'Windows SDK fxc.exe was not found.' }
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null
$tempRoot = Join-Path $artifactRoot 'temp'
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
try {
    $env:TEMP = $tempRoot
    $env:TMP = $tempRoot
    $binaryPath = Join-Path $artifactRoot 'ceiling_present.ps_2_0.cso'
    $logPath = Join-Path $artifactRoot 'ceiling_present-fxc.log'
    # Legacy sampler2D is required by ps_2_0; /Ges rejects it in current SDKs.
    & $FxcPath /nologo /O3 /T ps_2_0 /E main /Fo $binaryPath $sourcePath *> $logPath
    if ($LASTEXITCODE -ne 0) { throw "fxc failed; see $logPath" }
    $bytes = [IO.File]::ReadAllBytes($binaryPath)
    if ($bytes.Length -lt 8 -or $bytes.Length % 4 -ne 0 -or
        [BitConverter]::ToUInt32($bytes, 0) -ne 0xFFFF0200L -or
        [BitConverter]::ToUInt32($bytes, $bytes.Length - 4) -ne 0x0000FFFFL) {
        throw 'Compiler output is not a complete Direct3D9 ps_2_0 token stream.'
    }
    $sha = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('// Generated from ceiling_present.hlsl; do not edit bytecode by hand.')
    $lines.Add('// Reproduce: powershell -File tools/compile_ceiling_shader.ps1')
    $lines.Add('// fxc /O3 /T ps_2_0 /E main; Windows SDK 10.0.26100.0.')
    $lines.Add("// Bytecode SHA256: $sha")
    $lines.Add('#pragma once')
    $lines.Add('#include <d3d9.h>')
    $lines.Add('')
    $lines.Add('namespace gta2ceiling {')
    $lines.Add('static const DWORD kCeilingPresentShader[] = {')
    for ($offset = 0; $offset -lt $bytes.Length; $offset += 24) {
        $words = [Collections.Generic.List[string]]::new()
        for ($index = $offset; $index -lt [Math]::Min($offset + 24, $bytes.Length); $index += 4) {
            $words.Add(('0x{0:X8}u' -f [BitConverter]::ToUInt32($bytes, $index)))
        }
        $lines.Add('    ' + ($words -join ', ') + ',')
    }
    $lines.Add('};')
    $lines.Add('} // namespace gta2ceiling')
    [IO.File]::WriteAllText($headerPath, ($lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
    Write-Output "ps_2_0 bytecode generated ($($bytes.Length) bytes); SHA256 $sha"
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}
