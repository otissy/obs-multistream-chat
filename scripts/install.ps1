<#
 Installs obs-multistream-chat into the per-plugin ProgramData layout OBS scans, only when YOU run it:
   C:\ProgramData\obs-studio\plugins\obs-multistream-chat\bin\64bit\obs-multistream-chat.dll
   C:\ProgramData\obs-studio\plugins\obs-multistream-chat\data\{locale,overlay}
 Refuses to run while OBS is open (it would hold the old DLL locked). Close OBS first.
   powershell -File scripts\install.ps1               # DLL from the central build folder
   powershell -File scripts\install.ps1 -Uninstall
 -Dll / -Dest exist so the script can be tested against a scratch folder.
#>
[CmdletBinding()]
param([string]$Dll, [string]$Dest = (Join-Path $env:ProgramData 'obs-studio\plugins\obs-multistream-chat'), [switch]$Uninstall, [switch]$IgnoreRunningObs, [switch]$RemoveOld, [string]$BuildRoot)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\vsenv.ps1"

if (-not $IgnoreRunningObs -and (Get-Process obs64 -ErrorAction SilentlyContinue)) { throw 'OBS is running. Close it and run this again.' }

# The project was renamed from 'obs-multistream'. A leftover old install would load two docks.
$OldDest = Join-Path (Split-Path -Parent $Dest) 'obs-multistream'
if (Test-Path $OldDest) {
  $remove = $RemoveOld.IsPresent
  if (-not $remove) {
    Write-Host "Found an old install at $OldDest (the plugin was renamed to obs-multistream-chat)."
    try { $remove = ((Read-Host 'Remove it now? [y/N]') -match '^(y|yes)$') } catch { Write-Host 'Not removed (non-interactive). Re-run with -RemoveOld to delete it.' }
  }
  if ($remove) { Remove-Item $OldDest -Recurse -Force; Write-Host "removed $OldDest" }
}

if ($Uninstall) {
  if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force; Write-Host "removed $Dest" } else { Write-Host 'nothing to remove' }
  return
}
if (-not $Dll) { $Dll = (Get-ChildItem (Get-BuildRoot $BuildRoot) -Recurse -Filter obs-multistream-chat.dll -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
if (-not $Dll -or -not (Test-Path $Dll)) { throw 'obs-multistream-chat.dll not found. Run scripts\build.ps1 first (or pass -Dll).' }

$bin = Join-Path $Dest 'bin\64bit'
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item $Dll (Join-Path $bin 'obs-multistream-chat.dll') -Force
Copy-Item (Join-Path $Root 'data') (Join-Path $Dest 'data') -Recurse -Force
Write-Host "installed to $Dest"
Write-Host 'Start OBS, then View > Docks > OBS Multistream Chat.'
