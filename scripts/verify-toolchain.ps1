[CmdletBinding()]
param(
    [string]$QtRoot = $env:LISTENFREE_QT_ROOT
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'enter-dev-shell.ps1') -QtRoot $QtRoot -Quiet

$moduleNames = @('Core', 'Gui', 'Network', 'Qml', 'Quick', 'Sql', 'Multimedia', 'Test')
$missingModules = @()

foreach ($moduleName in $moduleNames) {
    $modulePath = Join-Path $env:LISTENFREE_QT_ROOT "lib\cmake\Qt6$moduleName"
    if (-not (Test-Path -LiteralPath $modulePath -PathType Container)) {
        $missingModules += $moduleName
    }
}

if ($missingModules.Count -gt 0) {
    throw "Qt modules missing from '$env:LISTENFREE_QT_ROOT': $($missingModules -join ', ')"
}

$qtVersion = (& qtpaths6 --qt-version).Trim()
$cmakeVersion = (& cmake --version | Select-Object -First 1).Trim()
$ninjaVersion = (& ninja --version).Trim()
$compilerVersion = (& g++ --version | Select-Object -First 1).Trim()

Write-Output "Qt=$qtVersion"
Write-Output "CMake=$cmakeVersion"
Write-Output "Ninja=$ninjaVersion"
Write-Output "Compiler=$compilerVersion"
Write-Output "QtModules=$($moduleNames -join ',')"
Write-Output 'ToolchainVerification=PASS'
