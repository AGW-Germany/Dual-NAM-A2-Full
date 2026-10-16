<#
.SYNOPSIS
  Runs every automated test against the built plug-in and writes the logs to
  test-output\. Run build.ps1 first.

.DESCRIPTION
  1. tests\make_test_assets.py      -> tests\generated\ (models / IRs derived from tests\models)
  2. tools\coretest\slimtest         A2 container + SetSlimmableSize(1.0) == A2 Full (8 ch, fast path)
     tools\coretest\thresholdproof   proof of the 1.2.0 gate threshold migration (old AudioDSPTools gate)
  3. tools\vst3host\dualtests        checks against the real .vst3 through the VST3 API
  4. tools\vst3host\guishot          opens the real editor, screenshots, clicks
  5. Steinberg validator             normal and extensive (-e) mode

.PARAMETER Quick
  Shorter storm / CPU runs for dualtests.
.PARAMETER SkipValidator
  Do not clone / build / run the Steinberg validator.
#>
param(
  [switch]$Quick,
  [switch]$SkipValidator
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Out = Join-Path $Root 'test-output'
New-Item -ItemType Directory -Force $Out | Out-Null
$plugBundle = Join-Path $Root 'NeuralAmpModeler\build-win\DualNAM_A2_Full.vst3'
$plugBinary = Join-Path $plugBundle 'Contents\x86_64-win\DualNAM_A2_Full.vst3'
if (-not (Test-Path $plugBinary)) { throw 'Plug-in not built: run build.ps1 first.' }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
function InVcEnv($cmd) {
  cmd /c "`"$vcvars`" >nul 2>&1 && $cmd"
  if ($LASTEXITCODE -ne 0) { throw "failed: $cmd" }
}
$results = [ordered]@{}
function Run($name, $exe, [string[]]$argList) {
  Write-Host "`n== $name" -ForegroundColor Cyan
  $log = Join-Path $Out "$name.log"
  # The tests deliberately make the plug-in print errors to stderr (broken
  # files, corrupt state). Windows PowerShell turns redirected stderr lines into
  # error records, which must not abort the run; the exit code decides.
  $ErrorActionPreference = 'Continue'
  & $exe @argList 2>&1 | Tee-Object -FilePath $log | Out-Host
  $results[$name] = $LASTEXITCODE
}

Write-Host '== test assets' -ForegroundColor Cyan
python -I (Join-Path $Root 'tests\make_test_assets.py')
if ($LASTEXITCODE -ne 0) { throw 'make_test_assets.py failed' }

Write-Host '== build test tools' -ForegroundColor Cyan
InVcEnv "`"$Root\tools\coretest\build_slimtest.bat`""
InVcEnv "`"$Root\tools\coretest\build_thresholdproof.bat`""
InVcEnv "`"$Root\tools\vst3host\build_win.bat`" dualtests.cpp dualtests"
InVcEnv "`"$Root\tools\vst3host\build_win.bat`" guishot.cpp guishot"

Run 'slimtest' "$Root\tools\coretest\slimtest.exe" @("$Root\tests\models\A2.nam", "$Root\tests\generated\A2_full_only.nam", "$Root\tests\generated\A2_lite_only.nam")
Run 'thresholdproof' "$Root\tools\coretest\thresholdproof.exe" @()
$dualArgs = @($plugBinary, $Root)
if ($Quick) { $dualArgs += '--quick' }
Run 'dualtests' "$Root\tools\vst3host\dualtests.exe" $dualArgs
Run 'guishot' "$Root\tools\vst3host\guishot.exe" @($plugBinary, $Root, (Join-Path $Out 'gui'))
Run 'guishot_monobus' "$Root\tools\vst3host\guishot.exe" @($plugBinary, $Root, (Join-Path $Out 'gui_monobus'), '--mono-bus')
Add-Type -AssemblyName System.Drawing
Get-ChildItem (Join-Path $Out 'gui*\*.bmp') -ErrorAction SilentlyContinue | ForEach-Object {
  $img = [System.Drawing.Image]::FromFile($_.FullName)
  $img.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
  $img.Dispose()
  Remove-Item $_.FullName
}

if (-not $SkipValidator) {
  # The plug-in build uses a trimmed SDK (iPlug2's download script deletes the
  # samples), so the validator is built from a separate full checkout of the same tag.
  $sdk = Join-Path $Root '_reference\vst3sdk'
  $validator = Join-Path $sdk 'build\bin\Release\validator.exe'
  if (-not (Test-Path $validator)) {
    New-Item -ItemType Directory -Force (Join-Path $Root '_reference') | Out-Null
    if (-not (Test-Path $sdk)) {
      git clone --branch v3.8.0_build_66 --depth 1 --recurse-submodules --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git $sdk
    }
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
      $cmake = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    }
    if (-not $cmake) { throw 'cmake not found (winget install Kitware.CMake)' }
    & $cmake -S $sdk -B "$sdk\build" -G 'Visual Studio 17 2022' -A x64 -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF -DSMTG_ENABLE_VST3_HOSTING_EXAMPLES=ON -DSMTG_CREATE_PLUGIN_LINK=OFF
    & $cmake --build "$sdk\build" --config Release --target validator
  }
  Run 'validator' $validator @($plugBundle)
  Run 'validator_extensive' $validator @('-e', $plugBundle)
  # The shipped bundle carries the version in its name ("Dual NAM A2 Full <ver>.vst3").
  $distBundle = Get-ChildItem (Join-Path $Root 'dist') -Directory -Filter 'Dual NAM A2 Full *.vst3' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($distBundle) { Run 'validator_dist' $validator @($distBundle.FullName) }
}

Write-Host "`n== summary" -ForegroundColor Cyan
$failed = 0
foreach ($k in $results.Keys) {
  $ok = $results[$k] -eq 0
  if (-not $ok) { $failed++ }
  Write-Host ("  {0,-22} {1}" -f $k, $(if ($ok) { 'OK' } else { "FAILED (exit $($results[$k]))" }))
}
Write-Host "logs: $Out"
exit $failed
