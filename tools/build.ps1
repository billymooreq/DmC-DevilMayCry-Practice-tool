# Builds dinput8.dll (x86) with the VS 2022 Build Tools toolchain.
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Config Release|RelWithDebInfo|Debug] [-Clean]
param(
  [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release',
  [switch]$Clean,
  # Test build that treats the game as an unknown build (build\<Config>-unknown\dinput8.dll).
  [switch]$SimulateUnknown
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'VS Build Tools with the C++ x86/x64 toolset not found' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeDir 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeDir 'Ninja\ninja.exe'
$build = Join-Path $root "build\$Config"
$sim = 'OFF'
if ($SimulateUnknown) { $build += '-unknown'; $sim = 'ON' }
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$cmd = "call `"$devcmd`" -arch=x86 -host_arch=x64 -no_logo && " +
       "`"$cmake`" -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config " +
       "-DDMCPT_SIMULATE_UNKNOWN_BUILD=$sim && " +
       "`"$cmake`" --build `"$build`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built: $build\dinput8.dll"
