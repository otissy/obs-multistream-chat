<#
 Builds obs-multistream-chat (plugin DLL + core tests) with Visual Studio 2022 / Build Tools 2022.
   powershell -File scripts\build.ps1            # plugin + tests
   powershell -File scripts\build.ps1 -Test      # ... then run the core tests
 All output goes under build\ (one folder, gitignored): exclude that folder in your antivirus if it
 flags fresh unsigned binaries (see README, "Antivirus").
 Reads (never loads) obs.dll / obs-frontend-api.dll from your OBS install to make import libraries;
 use -ObsBin to point at a different copy, e.g. a portable OBS.
#>
[CmdletBinding()]
param([switch]$Test, [string]$Configuration = 'Release', [string]$ObsBin = 'C:\Program Files\obs-studio\bin\64bit')
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\vsenv.ps1"
Enter-VsDevEnvironment
Set-Location $Root

if (-not (Test-Path '.obs-sdk\libobs\obs-module.h') -or -not (Test-Path '.qt-sdk\lib\Qt6Widgets.lib')) { & "$PSScriptRoot\fetch-sdks.ps1" }
foreach ($pair in @(@('obs', 'obs'), @('obs-frontend-api', 'obs-frontend-api'))) {
  $lib = ".obs-sdk\lib\$($pair[1]).lib"
  if (-not (Test-Path $lib)) { & "$PSScriptRoot\make-import-lib.ps1" -Dll (Join-Path $ObsBin "$($pair[0]).dll") -Name $pair[1] -OutDir '.obs-sdk\lib' }
}

cmake --preset windows-x64
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
cmake --build --preset windows-x64
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$dll = Get-ChildItem build\windows-x64 -Recurse -Filter obs-multistream-chat.dll | Select-Object -First 1
if (-not $dll) { throw 'obs-multistream-chat.dll was not produced' }
Write-Host "built $($dll.FullName)"

if ($Test) {
  $exe = Get-ChildItem build\windows-x64 -Recurse -Filter oms_tests.exe | Select-Object -First 1
  if (-not $exe) { throw 'oms_tests.exe missing' }
  & $exe.FullName all
  if ($LASTEXITCODE -ne 0) { throw 'core tests failed' }
  Write-Host 'core tests passed'
}
