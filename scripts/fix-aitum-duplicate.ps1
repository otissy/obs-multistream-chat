<#
.SYNOPSIS
  Removes the duplicate legacy Aitum Stream Suite DLL from C:\Program Files\obs-studio\obs-plugins\64bit\
  leaving the proper installation in C:\ProgramData\obs-studio\plugins\aitum-stream-suite\ intact.
#>
[CmdletBinding()]
param(
  [switch]$BackupOnly
)

$ErrorActionPreference = 'Stop'

$progFilesDll = "C:\Program Files\obs-studio\obs-plugins\64bit\aitum-stream-suite.dll"
$progFilesPdb = "C:\Program Files\obs-studio\obs-plugins\64bit\aitum-stream-suite.pdb"
$properInstall = "C:\ProgramData\obs-studio\plugins\aitum-stream-suite\bin\64bit\aitum-stream-suite.dll"
$backupDir = "$env:ProgramData\obs-studio\plugins\_backup_aitum_duplicate"

if (-not (Test-Path $progFilesDll)) {
  Write-Host "No duplicate Aitum DLL found in Program Files. Everything is clean!" -ForegroundColor Green
  exit 0
}

if (-not (Test-Path $properInstall)) {
  Write-Warning "The ProgramData installation was not found at $properInstall!"
  Write-Warning "Aborting to prevent deleting the only copy of Aitum Stream Suite."
  exit 1
}

# Check for admin privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
  Write-Host "Elevated administrator privileges required to modify C:\Program Files\obs-studio\." -ForegroundColor Yellow
  Write-Host "Requesting elevation..."
  $scriptPath = $MyInvocation.MyCommand.Path
  Start-Process powershell -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -NoProfile -File `"$scriptPath`""
  exit 0
}

# Running as Admin:
if (-not (Test-Path $backupDir)) {
  New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
}

Write-Host "Backing up duplicate to $backupDir..." -ForegroundColor Cyan
Copy-Item $progFilesDll -Destination $backupDir -Force
if (Test-Path $progFilesPdb) {
  Copy-Item $progFilesPdb -Destination $backupDir -Force
}

if (-not $BackupOnly) {
  Write-Host "Removing duplicate from Program Files..." -ForegroundColor Cyan
  Remove-Item $progFilesDll -Force
  if (Test-Path $progFilesPdb) {
    Remove-Item $progFilesPdb -Force
  }
  Write-Host "Successfully removed duplicate Aitum Stream Suite files from Program Files!" -ForegroundColor Green
}

Write-Host "The correct installation in $properInstall remains active." -ForegroundColor Green
