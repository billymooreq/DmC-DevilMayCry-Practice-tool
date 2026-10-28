# Builds the Release DLL and packs build\dist\DmCPracticeTool_v<version>.zip = dinput8.dll + README.txt + LICENSE
# (no PDB). The version comes from kToolVersion in src\version.h. Prints the SHA-256 of the zip and the dll.
#   .\tools\make_release.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$m = Select-String -Path (Join-Path $root 'src\version.h') -Pattern 'kToolVersion\[\] = "([^"]+)"'
if (-not $m) { throw 'kToolVersion not found in src\version.h' }
$ver = $m.Matches[0].Groups[1].Value
if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Config Release }
$dll = Join-Path $root 'build\Release\dinput8.dll'
if (-not (Test-Path $dll)) { throw "not built: $dll" }

$dist = Join-Path $root 'build\dist'
$stage = Join-Path $dist "DmCPracticeTool_v$ver"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll $stage
$readme = [IO.File]::ReadAllText((Join-Path $root 'release\README.txt')).Replace('{VERSION}', $ver)
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"), (New-Object Text.UTF8Encoding $false))
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')

$zip = Join-Path $dist "DmCPracticeTool_v$ver.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "zip: $zip"
Write-Host ("SHA-256 zip:         " + (Get-FileHash $zip -Algorithm SHA256).Hash)
Write-Host ("SHA-256 dinput8.dll: " + (Get-FileHash $dll -Algorithm SHA256).Hash)
