<#
 Makes dist\obs-multistream-<version>-windows-x64.zip (+ .sha256), laid out so extracting it into
 C:\ProgramData\obs-studio\plugins\ gives the same tree install.ps1 creates.
   powershell -File scripts\package.ps1 [-Dll path] [-OutDir dist]
#>
[CmdletBinding()]
param([string]$Dll, [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist'))
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

$cm = Get-Content (Join-Path $Root 'CMakeLists.txt') -Raw
if ($cm -notmatch 'project\(obs-multistream VERSION (\d+\.\d+\.\d+)') { throw 'cannot read version from CMakeLists.txt' }
$ver = $Matches[1]
if (-not $Dll) { $Dll = (Get-ChildItem (Join-Path $Root 'build') -Recurse -Filter obs-multistream.dll -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
if (-not $Dll -or -not (Test-Path $Dll)) { throw 'obs-multistream.dll not found. Run scripts\build.ps1 first (or pass -Dll).' }

$stage = Join-Path $env:TEMP ("oms-package-" + [guid]::NewGuid().ToString('N'))
$tree = Join-Path $stage 'obs-multistream'
New-Item -ItemType Directory -Force (Join-Path $tree 'bin\64bit') | Out-Null
Copy-Item $Dll (Join-Path $tree 'bin\64bit\obs-multistream.dll')
Copy-Item (Join-Path $Root 'data') (Join-Path $tree 'data') -Recurse
foreach ($f in 'README.md', 'LICENSE', 'NOTICE', 'CHANGELOG.md') { Copy-Item (Join-Path $Root $f) $tree }
Copy-Item (Join-Path $Root 'docs') (Join-Path $tree 'docs') -Recurse

New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path $OutDir "obs-multistream-$ver-windows-x64.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $tree -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash *$(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ascii
Remove-Item $stage -Recurse -Force
Write-Host "package: $zip"
Write-Host "sha-256: $hash"
