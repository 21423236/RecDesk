param(
  [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
  [string]$Ffmpeg = '',
  [switch]$SkipTests,
  [switch]$TestOnly
)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$build = Join-Path $source 'build'
$release = Join-Path $source 'release'
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmake) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path $vswhere) {
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($installation) { $cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
  }
}
if (-not $cmake -or -not (Test-Path $cmake)) { throw 'Install Visual Studio C++ desktop tools and CMake 3.24+ first.' }
if ($TestOnly) {
  if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) { throw 'Build native/ first.' }
  & $cmake --build $build --config $Configuration --target RUN_TESTS
  if ($LASTEXITCODE) { throw 'Native tests failed.' }
  exit 0
}
if (-not $Ffmpeg) {
  $ffmpegCommand = Get-Command ffmpeg -ErrorAction SilentlyContinue
  $legacyBinary = Join-Path $source '..\node_modules\ffmpeg-static\ffmpeg.exe'
  if ($ffmpegCommand) { $Ffmpeg = $ffmpegCommand.Source }
  elseif (Test-Path $legacyBinary) { $Ffmpeg = (Resolve-Path $legacyBinary).Path }
}
if (-not $Ffmpeg -or -not (Test-Path $Ffmpeg)) { throw 'Pass -Ffmpeg C:\path\to\ffmpeg.exe (with gdigrab, libx264 and aac support).' }
$Ffmpeg = (Resolve-Path $Ffmpeg).Path
& $cmake -S $source -B $build -A x64 "-DRECDESK_FFMPEG=$Ffmpeg" '-DBUILD_TESTING=ON'
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
& $cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'C++ build failed.' }
if (-not $SkipTests) {
  & $cmake --build $build --config $Configuration --target RUN_TESTS
  if ($LASTEXITCODE) { throw 'Core tests failed.' }
}
& $cmake --install $build --config $Configuration --prefix $release
if ($LASTEXITCODE) { throw 'Packaging failed.' }
& $Ffmpeg -hide_banner -L | Set-Content -Encoding UTF8 (Join-Path $release 'FFMPEG-LICENSE.txt')
if ($LASTEXITCODE) { throw 'Could not read FFmpeg license.' }
Write-Host "Ready: $release\RecDesk.exe"
