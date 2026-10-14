<#
 Installs obs-multistream into the per-plugin ProgramData layout OBS scans, only when YOU run it:
   C:\ProgramData\obs-studio\plugins\obs-multistream\bin\64bit\obs-multistream.dll
   C:\ProgramData\obs-studio\plugins\obs-multistream\data\{locale,overlay}
 Refuses to run while OBS is open (it would hold the old DLL locked). Close OBS first.
   powershell -File scripts\install.ps1               # from build\
   powershell -File scripts\install.ps1 -Uninstall
 -Dll / -Dest exist so the script can be tested against a scratch folder.
#>
[CmdletBinding()]
param([string]$Dll, [string]$Dest = (Join-Path $env:ProgramData 'obs-studio\plugins\obs-multistream'), [switch]$Uninstall, [switch]$IgnoreRunningObs)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

if (-not $IgnoreRunningObs -and (Get-Process obs64 -ErrorAction SilentlyContinue)) { throw 'OBS is running. Close it and run this again.' }

if ($Uninstall) {
  if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force; Write-Host "removed $Dest" } else { Write-Host 'nothing to remove' }
  return
}
if (-not $Dll) { $Dll = (Get-ChildItem (Join-Path $Root 'build') -Recurse -Filter obs-multistream.dll -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
if (-not $Dll -or -not (Test-Path $Dll)) { throw 'obs-multistream.dll not found. Run scripts\build.ps1 first (or pass -Dll).' }

$bin = Join-Path $Dest 'bin\64bit'
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item $Dll (Join-Path $bin 'obs-multistream.dll') -Force
Copy-Item (Join-Path $Root 'data') (Join-Path $Dest 'data') -Recurse -Force
Write-Host "installed to $Dest"
Write-Host 'Start OBS, then View > Docks > Multistream.'
