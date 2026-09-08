[CmdletBinding()]
param(
    [string]$SourceRoot='',
    [string]$Version='0.3.1',
    [string]$OutputDirectory='',
    [string]$InnoCompiler='C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    [string]$SevenZip='C:\Program Files\7-Zip\7z.exe'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if (!$SourceRoot) { $SourceRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?$') { throw 'Invalid release version' }
$source=[IO.Path]::GetFullPath($SourceRoot)
$runtime=Join-Path $source 'dist\ListenFree-Portable'
$releaseRoot=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $source "dist\releases\$Version"}
$stage=Join-Path $releaseRoot "ListenFree-$Version-windows-x64"
# A fresh directory prevents an earlier smoke test's data from entering a ZIP.
if (Test-Path -LiteralPath $stage) { throw "Release stage already exists: $stage" }
foreach ($required in $InnoCompiler,$SevenZip,(Join-Path $source 'build\portable\listenfree.exe'),(Join-Path $source 'packaging\usage.txt')) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing release input: $required" }
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
# Copy only deployed runtime binaries and known runtime directories. Never copy
# data/, loose JSON/INI, source scripts, histories, diagnostics or local media.
Get-ChildItem -LiteralPath $runtime -File -Filter '*.dll' | Copy-Item -Destination $stage
foreach ($name in 'generic','iconengines','imageformats','multimedia','networkinformation','platforms','qml','qmmp','sqldrivers','styles','tls') {
    $directory=Join-Path $runtime $name
    if (Test-Path -LiteralPath $directory) { Copy-Item -LiteralPath $directory -Destination $stage -Recurse }
}
foreach ($name in 'listenfree.exe','listenfree-sourcehost.exe') {
    Copy-Item -LiteralPath (Join-Path $source "build\portable\$name") -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $source '..\_vendor\qmmp-build-qt\src\plugins\Transports\http\http.dll') -Destination (Join-Path $stage 'qmmp\Transports\http.dll') -Force
# The tested runtime carries patched Qt/Qmmp DLLs; do not replace them with SDK originals.
Copy-Item -LiteralPath (Join-Path $runtime 'qt.conf') -Destination $stage
Copy-Item -LiteralPath (Join-Path $runtime 'licenses') -Destination $stage -Recurse
Copy-Item -Path (Join-Path $source 'licenses\*.txt') -Destination (Join-Path $stage 'licenses') -Force
foreach ($pair in @(@('.vcpkg_installed\x64-mingw-dynamic\share','qjs'),@('.vcpkg_installed\x64-mingw-dynamic\share','taglib'),@('.vcpkg_installed\x64-mingw-dynamic\share','zlib'),@('.vcpkg_installed\x64-mingw-dynamic\share','utf8cpp'),@('..\_vendor\qmmp-vcpkg-installed-qt\x64-mingw-dynamic\share','ffmpeg'),@('..\_vendor\qmmp-vcpkg-installed-qt\x64-mingw-dynamic\share','curl'))) {
    $copyright=Join-Path $source ($pair[0]+'\'+$pair[1]+'\copyright')
    if (Test-Path -LiteralPath $copyright) { Copy-Item -LiteralPath $copyright -Destination (Join-Path $stage ('licenses\'+$pair[1]+'-copyright.txt')) }
}
Copy-Item -LiteralPath (Join-Path $source 'licenses\THIRD-PARTY-NOTICES.txt') -Destination (Join-Path $stage 'licenses\THIRD-PARTY-NOTICES.txt') -Force
Copy-Item -LiteralPath (Join-Path $source 'packaging\usage.txt') -Destination (Join-Path $stage '使用说明.txt')
New-Item -ItemType File -Path (Join-Path $stage 'portable.mode') | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $stage -File -Recurse) {
    $relative=$file.FullName.Substring($stage.Length+1)
    if ($relative -match '(^|[\\/])(data|cache|logs|sources)([\\/]|$)' -or $file.Extension -in '.sqlite','.db','.log','.mp3','.flac','.m4a','.mp4') {
        throw "Unexpected personal-data candidate: $relative"
    }
}
$smokeData=Join-Path $source "build\release-smoke-$Version"
& (Join-Path $PSScriptRoot 'test-portable-startup.ps1') -PackageRoot $stage -DataDirectory $smokeData
& $InnoCompiler "/DStageDir=$stage" "/DOutputDir=$releaseRoot" "/DReleaseVersion=$Version" (Join-Path $PSScriptRoot 'listenfree.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
$zip=Join-Path $releaseRoot "ListenFree-$Version-windows-x64-Portable.zip"
& $SevenZip a -tzip -mx=7 $zip $stage
if ($LASTEXITCODE -ne 0) { throw 'Portable archive creation failed' }
& $SevenZip t $zip
if ($LASTEXITCODE -ne 0) { throw 'Portable archive integrity check failed' }
$artifacts=Get-ChildItem -LiteralPath $releaseRoot -File | Where-Object Extension -In '.exe','.zip'
$checksums=foreach ($file in $artifacts) { "{0}  {1}" -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$file.Name }
[IO.File]::WriteAllLines((Join-Path $releaseRoot 'SHA256SUMS.txt'),$checksums,[Text.UTF8Encoding]::new($false))
Write-Output "发布包：$releaseRoot"
