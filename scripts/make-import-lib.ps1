<#
 Generates an MSVC import library from an installed DLL's export table (no OBS SDK ships one).
 Reads the DLL file only; never loads or runs it.
   powershell -File scripts/make-import-lib.ps1 -Dll "C:\Program Files\obs-studio\bin\64bit\obs.dll" -Name obs
 OutDir defaults to <build root>\import-libs (D:\builds\obs-multistream-chat unless -BuildRoot or
 $env:OMSC_BUILD_ROOT says otherwise): generated binaries never go into the repo.
#>
param([Parameter(Mandatory)][string]$Dll, [Parameter(Mandatory)][string]$Name, [string]$OutDir, [string]$BuildRoot)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\vsenv.ps1"
Enter-VsDevEnvironment
if (-not $OutDir) { $OutDir = Join-Path (Get-BuildRoot $BuildRoot) 'import-libs' }
if (-not (Test-Path $Dll)) { throw "no such DLL: $Dll" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$def = Join-Path $OutDir "$Name.def"
$lines = @("LIBRARY $Name", 'EXPORTS')
foreach ($l in (& dumpbin /nologo /exports $Dll)) {
  if ($l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $lines += "    $($Matches[1])" }
}
if ($lines.Count -lt 10) { throw "dumpbin found too few exports in $Dll" }
Set-Content -Path $def -Value $lines -Encoding ascii
& lib /nologo "/def:$def" /machine:x64 "/out:$(Join-Path $OutDir "$Name.lib")"
if ($LASTEXITCODE -ne 0) { throw 'lib.exe failed' }
Write-Host "generated $(Join-Path $OutDir "$Name.lib") ($($lines.Count - 2) exports)"
