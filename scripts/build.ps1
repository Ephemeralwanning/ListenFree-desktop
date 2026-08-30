[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'enter-dev-shell.ps1') -Quiet

$preset = "windows-$($Configuration.ToLowerInvariant())"
cmake --preset $preset
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $preset" }
cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $preset" }
if (-not $SkipTests) {
    ctest --test-dir "build\windows-$($Configuration.ToLowerInvariant())" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed for $preset" }
}
