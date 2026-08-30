[CmdletBinding()]
param(
    [string]$QtRoot = $env:LISTENFREE_QT_ROOT,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    $QtRoot = 'F:\qt\6.10.3\mingw_64'
}

$toolRoots = [ordered]@{
    Qt = $QtRoot
    CMake = 'F:\qt\Tools\CMake_64\bin'
    Ninja = 'F:\qt\Tools\Ninja'
    Compiler = 'F:\qt\Tools\mingw1310_64\bin'
}

foreach ($entry in $toolRoots.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Container)) {
        throw "ListenFree toolchain component '$($entry.Key)' was not found at '$($entry.Value)'."
    }
}

$requiredFiles = @(
    (Join-Path $toolRoots.Qt 'bin\qtpaths6.exe'),
    (Join-Path $toolRoots.CMake 'cmake.exe'),
    (Join-Path $toolRoots.Ninja 'ninja.exe'),
    (Join-Path $toolRoots.Compiler 'g++.exe')
)

foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required ListenFree build tool was not found: '$requiredFile'."
    }
}

$env:LISTENFREE_QT_ROOT = $toolRoots.Qt
$env:QT_ROOT = $toolRoots.Qt
$env:CMAKE_PREFIX_PATH = $toolRoots.Qt
$env:QT_HOST_PATH = $toolRoots.Qt

$pathEntries = @(
    (Join-Path $toolRoots.Qt 'bin'),
    $toolRoots.CMake,
    $toolRoots.Ninja,
    $toolRoots.Compiler
)

$existingEntries = $env:Path -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
$env:Path = (($pathEntries + $existingEntries) | Select-Object -Unique) -join ';'

if (-not $Quiet) {
    Write-Output 'ListenFree development shell configured.'
    Write-Output "Qt root: $($toolRoots.Qt)"
    Write-Output "Generator: Ninja"
    Write-Output "Compiler: MinGW x64"
}
