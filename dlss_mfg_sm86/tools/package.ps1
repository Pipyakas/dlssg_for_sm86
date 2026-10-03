# Zips build\release\ (built by build.cmd) into dist\dlss_mfg_sm86.zip, the same
# layout dlssg_sm86 ships: proxies + INI at the root, alternatives\ beside them.
# Per-game files are already copy-ready in games\<category>\<game>\mod\.
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$release = Join-Path $project 'build\release'
foreach ($name in @('winmm.dll', 'dinput8.dll', 'dbghelp.dll', 'dlss_mfg_sm86.ini', 'alternatives\dxgi.dll', 'alternatives\d3d12.dll', 'alternatives\version.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $release $name))) { throw "Build first: missing $name" }
}
$dist = Join-Path $project 'dist'
if (-not (Test-Path -LiteralPath $dist)) { New-Item -ItemType Directory -Path $dist > $null }
$zip = Join-Path $dist 'dlss_mfg_sm86.zip'
Compress-Archive -Path (Join-Path $release '*') -DestinationPath $zip -Force
Get-ChildItem -LiteralPath $release -Recurse -File | Get-FileHash | Format-Table Hash, Path -AutoSize
"Packaged $zip"
