<#
 Makes obs-multistream-chat-<version>-windows-x64.zip and setup .exe (+ .sha256).
   powershell -File scripts\package.ps1 [-Dll path] [-OutDir dir] [-BuildRoot dir] [-Voices dir] [-PushRelease]
 -Voices: the --out folder of scripts\fetch-voices.js (licence-checked, pinned voice packs)
 Default output: <build root>\dist and D:\build
#>
[CmdletBinding()]
param([string]$Dll, [string]$OutDir, [string]$BuildRoot, [string]$Voices, [switch]$PushRelease, [switch]$SkipInstaller, [switch]$NoAltCopy)
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
# OBS 33+ layout as well (https://obsproject.com/kb/legacy-plugin-locations): OBS 32 ignores this copy,
# OBS 33 loads it and skips bin\64bit, so the same zip installs on either version
Copy-Item $Dll (Join-Path $tree 'obs-multistream-chat.dll')
Copy-Item (Join-Path $Root 'data') (Join-Path $tree 'data') -Recurse
foreach ($f in 'README.md', 'LICENSE', 'NOTICE', 'CHANGELOG.md') { Copy-Item (Join-Path $Root $f) $tree }
Copy-Item (Join-Path $Root 'docs') (Join-Path $tree 'docs') -Recurse
# the chat reader (plain JavaScript + selector packs) lives next to the data files: obs_module_file("reader/index.js")
Copy-Item (Join-Path $Root 'reader') (Join-Path $tree 'data\reader') -Recurse

# the voice helper (plain JavaScript) plus its runtime from npm: sherpa-onnx-node and the Windows native
# package (sherpa-onnx-c-api.dll, onnxruntime.dll), pinned by registry integrity and laid out as node_modules
Copy-Item (Join-Path $Root 'voice') (Join-Path $tree 'data\voice') -Recurse
$npmPins = @(
  @{ name = 'sherpa-onnx-node'; ver = '1.13.8'; sri = 'sha512-MsDMBdhLFTZ1GwvcGSSQhnS7g/EA8OMH6IYysCVUOM7j8Icty9KRc0E6YT1A5fWBsZwRfKOeh88QC95aRvS8ag==' },
  @{ name = 'sherpa-onnx-win-x64'; ver = '1.13.8'; sri = 'sha512-oZF1c9VPOKtMwn83Bboc5XSWL+76BRoyB3eUuVnCknBKxwSULZU2Foia9VHWzU+n4I12rPsP6z6H9Rp1hD9o8g==' }
)
$npmCache = Join-Path $Central 'npm-cache'
New-Item -ItemType Directory -Force $npmCache | Out-Null
foreach ($p in $npmPins) {
  $tgz = Join-Path $npmCache "$($p.name)-$($p.ver).tgz"
  if (-not (Test-Path $tgz)) {
    curl.exe -sSfL -o $tgz "https://registry.npmjs.org/$($p.name)/-/$($p.name)-$($p.ver).tgz"
    if ($LASTEXITCODE -ne 0) { throw "could not download $($p.name) $($p.ver)" }
  }
  $sha = [System.Security.Cryptography.SHA512]::Create()
  $sri = 'sha512-' + [Convert]::ToBase64String($sha.ComputeHash([System.IO.File]::ReadAllBytes($tgz)))
  if ($sri -ne $p.sri) { Remove-Item $tgz -Force; throw "$($p.name) $($p.ver): integrity mismatch ($sri)" }
  $dest = Join-Path $tree "data\voice\node_modules\$($p.name)"
  New-Item -ItemType Directory -Force $dest | Out-Null
  # Windows' own bsdtar by full path: from Git Bash, GNU tar comes first on PATH and reads 'C:\...' as host:path
  & (Join-Path $env:SystemRoot 'System32\tar.exe') -xzf $tgz -C $dest --strip-components=1
  if ($LASTEXITCODE -ne 0) { throw "could not unpack $($p.name)" }
}

# voice packs that scripts\fetch-voices.js shipped: the embedded one (Kokoro) goes in data\voices (zip and setup alike),
# the rest become download components of the setup (packaging\voices.generated.iss)
$voicesIss = Join-Path $Root 'packaging\voices.generated.iss'
Remove-Item $voicesIss -Force -ErrorAction SilentlyContinue
if ($Voices) {
  $manifest = Join-Path $Voices 'manifest.json'
  if (-not (Test-Path $manifest)) { throw "no manifest.json in ${Voices}: run scripts\fetch-voices.js first" }
  $gen = Join-Path $Root 'scripts\voice-installer.js'
  node $gen --manifest $manifest --out $voicesIss
  if ($LASTEXITCODE -ne 0) { throw 'voice-installer.js failed' }
  $shipped = (Get-Content $manifest -Raw | ConvertFrom-Json).shipped | ForEach-Object { $_.id }
  $embed = (node -e "process.stdout.write(require(process.argv[1]).DEFAULT_EMBED.join(','))" $gen).Split(',')
  New-Item -ItemType Directory -Force (Join-Path $tree 'data\voices') | Out-Null
  foreach ($id in $embed) {
    if ($shipped -contains $id) { Copy-Item (Join-Path $Voices $id) (Join-Path $tree "data\voices\$id") -Recurse }
    # a setup without its included voices would install fine and then never speak: stop the build instead
    else { throw "embedded voice pack $id was not shipped by fetch-voices (see manifest.json in $Voices)" }
  }
  Copy-Item (Join-Path $Voices 'LICENCES.md') (Join-Path $tree 'docs\voice-licences.md')
} else {
  Write-Warning 'no -Voices folder: the package has no voice packs (the dock will say none are installed)'
}

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

# Also copy artifacts to D:\build (-NoAltCopy: tests, so a fake package never lands next to real releases)
$altDir = "D:\build"
if (-not $NoAltCopy) { try {
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
} }

Remove-Item $stage -Recurse -Force
