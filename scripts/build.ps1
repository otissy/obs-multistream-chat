<#
 Builds obs-multistream-chat (plugin DLL + core tests) with Visual Studio 2022 / Build Tools 2022.
   powershell -File scripts\build.ps1            # plugin + tests
   powershell -File scripts\build.ps1 -Test      # ... then run the core tests
 ALL compiled output goes under one central folder, default D:\builds\obs-multistream-chat\
 (override with -BuildRoot or $env:OMSC_BUILD_ROOT): CMake tree, test exes, DLL, import libs, zip.
 Exclude that one folder in your antivirus if it flags fresh unsigned binaries (README, "Antivirus").
 Anything placed there skips scanning, so only build code you have reviewed.
 Reads (never loads) obs.dll / obs-frontend-api.dll from your OBS install to make import libraries;
 use -ObsBin to point at a different copy, e.g. a portable OBS.
#>
[CmdletBinding()]
param([switch]$Test, [string]$Configuration = 'Release', [string]$ObsBin = 'C:\Program Files\obs-studio\bin\64bit', [string]$BuildRoot)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\vsenv.ps1"
Enter-VsDevEnvironment
$Out = Get-BuildRoot $BuildRoot
$ImportLibs = Join-Path $Out 'import-libs'
Set-Location $Root

if (-not (Test-Path '.obs-sdk\libobs\obs-module.h') -or -not (Test-Path '.qt-sdk\lib\Qt6Widgets.lib')) { & "$PSScriptRoot\fetch-sdks.ps1" }
foreach ($name in @('obs', 'obs-frontend-api')) {
  $lib = Join-Path $ImportLibs "$name.lib"
  if (-not (Test-Path $lib)) { & "$PSScriptRoot\make-import-lib.ps1" -Dll (Join-Path $ObsBin "$name.dll") -Name $name -OutDir $ImportLibs }
}

$Tree = Join-Path $Out 'windows-x64'
cmake -S . -B $Tree -G 'Visual Studio 17 2022' -A x64 -DOMS_BUILD_PLUGIN=ON -DOMS_BUILD_TESTS=ON "-DOMS_IMPORT_LIB_DIR=$ImportLibs"
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
cmake --build $Tree --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$dll = Get-ChildItem $Tree -Recurse -Filter obs-multistream-chat.dll | Select-Object -First 1
if (-not $dll) { throw 'obs-multistream-chat.dll was not produced' }
Write-Host "built $($dll.FullName)"

if ($Test) {
  $exe = Get-ChildItem $Tree -Recurse -Filter oms_tests.exe | Select-Object -First 1
  if (-not $exe) { throw 'oms_tests.exe missing' }
  & $exe.FullName all
  if ($LASTEXITCODE -ne 0) { throw 'core tests failed' }
  Write-Host 'core tests passed'
}
