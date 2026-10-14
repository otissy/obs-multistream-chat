# Imports the Visual Studio x64 build environment into this PowerShell session (vswhere + vcvars64).
function Enter-VsDevEnvironment {
  if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw 'Visual Studio Installer not found: install the Build Tools 2022 (Desktop development with C++).' }
  $root = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
  if (-not $root) { throw 'No Visual Studio with the C++ x64 tools found (Build Tools 2022 > Desktop development with C++).' }
  $bat = Join-Path $root 'VC\Auxiliary\Build\vcvars64.bat'
  $out = & cmd.exe /c "`"$bat`" >nul && set"
  foreach ($line in $out) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
}
