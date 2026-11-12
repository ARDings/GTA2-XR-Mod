# Compatibility entry point; build_mod.ps1 is the single validated build pipeline.
param(
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [switch]$RunTests = $true
)
$ErrorActionPreference = 'Stop'
if (-not $RunTests) {
    Write-Warning 'The packaged build always runs its required local checks.'
}
& (Join-Path $PSScriptRoot 'build_mod.ps1') -Configuration $Config
