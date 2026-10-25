<#
 Installs obs-multistream-chat into the per-plugin ProgramData layout OBS scans, only when YOU run it:
   C:\ProgramData\obs-studio\plugins\obs-multistream-chat\bin\64bit\obs-multistream-chat.dll   (OBS 32 and older)
   C:\ProgramData\obs-studio\plugins\obs-multistream-chat\obs-multistream-chat.dll             (OBS 33+)
   C:\ProgramData\obs-studio\plugins\obs-multistream-chat\data\{locale,overlay,reader}
 Refuses to run while OBS is open (it would hold the old DLL locked). Close OBS first.
   powershell -File scripts\install.ps1               # DLL from the central build folder
   powershell -File scripts\install.ps1 -Layout modern
   powershell -File scripts\install.ps1 -Uninstall
 -Layout both (default): the same DLL in both places. OBS 32 only loads bin\64bit; OBS 33 loads the
   top-level copy first and skips the legacy one (same module name), so the install survives an OBS
   upgrade. legacy / modern write one copy and delete the other layout's stale one.
   Layouts: https://obsproject.com/kb/legacy-plugin-locations
 -Dll / -Dest exist so the script can be tested against a scratch folder.
#>
[CmdletBinding()]
param([string]$Dll, [string]$Dest = (Join-Path $env:ProgramData 'obs-studio\plugins\obs-multistream-chat'), [switch]$Uninstall, [switch]$IgnoreRunningObs, [switch]$RemoveOld, [string]$BuildRoot,
  [ValidateSet('both', 'legacy', 'modern')][string]$Layout = 'both')
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
if (-not $Dll) { $Dll = (Get-ChildItem (Get-BuildRoot $BuildRoot) -Recurse -Filter obs-multistream-chat.dll -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName }
if (-not $Dll -or -not (Test-Path $Dll)) { throw 'obs-multistream-chat.dll not found. Run scripts\build.ps1 first (or pass -Dll).' }

$bin = Join-Path $Dest 'bin\64bit'
$legacyDll = Join-Path $bin 'obs-multistream-chat.dll'
$modernDll = Join-Path $Dest 'obs-multistream-chat.dll'
New-Item -ItemType Directory -Force $Dest | Out-Null
if ($Layout -ne 'modern') {
  New-Item -ItemType Directory -Force $bin | Out-Null
  Copy-Item $Dll $legacyDll -Force
}
if ($Layout -ne 'legacy') { Copy-Item $Dll $modernDll -Force }
# one layout only: drop the other layout's copy so a stale DLL can't load on the other OBS version
if ($Layout -eq 'legacy' -and (Test-Path $modernDll)) { Remove-Item $modernDll -Force }
if ($Layout -eq 'modern' -and (Test-Path $legacyDll)) {
  Remove-Item $legacyDll -Force
  foreach ($d in $bin, (Join-Path $Dest 'bin')) { if ((Test-Path $d) -and -not (Get-ChildItem $d -Force)) { Remove-Item $d -Force } }
}

$dataDest = Join-Path $Dest 'data'
if (Test-Path (Join-Path $dataDest 'data')) { Remove-Item (Join-Path $dataDest 'data') -Recurse -Force }
New-Item -ItemType Directory -Force $dataDest | Out-Null
Copy-Item (Join-Path $Root 'data\*') $dataDest -Recurse -Force

$readerDest = Join-Path $dataDest 'reader'
if (Test-Path (Join-Path $readerDest 'reader')) { Remove-Item (Join-Path $readerDest 'reader') -Recurse -Force }
New-Item -ItemType Directory -Force $readerDest | Out-Null
Copy-Item (Join-Path $Root 'reader\*') $readerDest -Recurse -Force
Write-Host "installed to $Dest ($Layout layout)"
Write-Host 'Start OBS, then View > Docks > OBS Multistream Chat.'
