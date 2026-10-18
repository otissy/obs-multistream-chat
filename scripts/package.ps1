<#
 Makes obs-multistream-chat-<version>-windows-x64.zip and setup .exe (+ .sha256).
   powershell -File scripts\package.ps1 [-Dll path] [-OutDir dir] [-BuildRoot dir] [-PushRelease]
 Default output: <build root>\dist and D:\build
#>
[CmdletBinding()]
param([string]$Dll, [string]$OutDir, [string]$BuildRoot, [switch]$PushRelease, [switch]$SkipInstaller)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\vsenv.ps1"
$Central = Get-BuildRoot $BuildRoot
if (-not $OutDir) { $OutDir = Join-Path $Central 'dist' }

$cm = Get-Content (Join-Path $Root 'CMakeLists.txt') -Raw
if ($cm -notmatch 'project\(obs-multistream-chat VERSION (\d+\.\d+\.\d+)') { throw 'cannot read version from CMakeLists.txt' }
$ver = $Matches[1]
if (-not $Dll) { $Dll = (Get-ChildItem $Central -Recurse -Filter obs-multistream-chat.dll -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName }
if (-not $Dll -or -not (Test-Path $Dll)) { throw 'obs-multistream-chat.dll not found. Run scripts\build.ps1 first (or pass -Dll).' }

$stage = Join-Path $env:TEMP ("oms-package-" + [guid]::NewGuid().ToString('N'))
$tree = Join-Path $stage 'obs-multistream-chat'
New-Item -ItemType Directory -Force (Join-Path $tree 'bin\64bit') | Out-Null
Copy-Item $Dll (Join-Path $tree 'bin\64bit\obs-multistream-chat.dll')
Copy-Item (Join-Path $Root 'data') (Join-Path $tree 'data') -Recurse
# the chat reader (plain JavaScript + selector packs) lives next to the data files: obs_module_file("reader/index.js")
Copy-Item (Join-Path $Root 'reader') (Join-Path $tree 'data\reader') -Recurse
foreach ($f in 'README.md', 'LICENSE', 'NOTICE', 'CHANGELOG.md') { Copy-Item (Join-Path $Root $f) $tree }
Copy-Item (Join-Path $Root 'docs') (Join-Path $tree 'docs') -Recurse

New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path $OutDir "obs-multistream-chat-$ver-windows-x64-windows-x64.zip"
# Standard name without duplicate windows-x64
$zip = Join-Path $OutDir "obs-multistream-chat-$ver-windows-x64.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $tree -DestinationPath $zip -CompressionLevel Optimal

function Get-Sha256Hex([string]$path) {
  if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
    return (Get-FileHash $path -Algorithm SHA256).Hash.ToLower()
  }
  $fs = [System.IO.File]::OpenRead($path)
  $sha = [System.Security.Cryptography.SHA256]::Create()
  $hash = [System.BitConverter]::ToString($sha.ComputeHash($fs)).Replace("-","").ToLower()
  $fs.Dispose()
  return $hash
}

$zipHash = Get-Sha256Hex $zip
"$zipHash *$(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ascii
Write-Host "package: $zip"
Write-Host "sha-256: $zipHash"

$setupExe = $null
if (-not $SkipInstaller) {
  $isccCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
  )
  $iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($iscc) {
    $issFile = Join-Path $Root 'packaging\installer.iss'
    if (Test-Path $issFile) {
      Write-Host "Compiling setup installer with Inno Setup..."
      & $iscc "/DSourceTree=$tree" "/DOutDir=$OutDir" "/DMyAppVersion=$ver" $issFile
      if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compiler failed' }
      $setupExe = Join-Path $OutDir "obs-multistream-chat-$ver-windows-x64-setup.exe"
      if (Test-Path $setupExe) {
        $exeHash = Get-Sha256Hex $setupExe
        "$exeHash *$(Split-Path $setupExe -Leaf)" | Set-Content "$setupExe.sha256" -Encoding ascii
        Write-Host "setup:   $setupExe"
        Write-Host "sha-256: $exeHash"
      }
    }
  } else {
    Write-Warning "Inno Setup (ISCC.exe) not found; skipping setup .exe compilation"
  }
}

# Also copy artifacts to D:\build
$altDir = "D:\build"
try {
  New-Item -ItemType Directory -Force $altDir -ErrorAction SilentlyContinue | Out-Null
  Copy-Item $zip (Join-Path $altDir (Split-Path $zip -Leaf)) -Force
  Copy-Item "$zip.sha256" (Join-Path $altDir (Split-Path "$zip.sha256" -Leaf)) -Force
  if ($setupExe -and (Test-Path $setupExe)) {
    Copy-Item $setupExe (Join-Path $altDir (Split-Path $setupExe -Leaf)) -Force
    Copy-Item "$setupExe.sha256" (Join-Path $altDir (Split-Path "$setupExe.sha256" -Leaf)) -Force
  }
  Write-Host "Artifacts also copied to $altDir"
} catch {
  Write-Warning "Could not copy to ${altDir}: $_"
}

Remove-Item $stage -Recurse -Force
