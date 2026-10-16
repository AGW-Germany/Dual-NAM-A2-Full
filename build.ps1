<#
.SYNOPSIS
  Reproducible Windows x64 Release build of the Dual NAM A2 Full VST3, plus the
  dist/ folder and the installation ZIP.

.DESCRIPTION
  1. Checks the toolchain (Visual Studio 2022 Build Tools with MSVC v143, Windows SDK, Git).
  2. Initialises the git submodules (iPlug2, NeuralAmpModelerCore, AudioDSPTools, eigen).
  3. Downloads the Steinberg VST3 SDK at the pinned tag v3.8.0_build_66 if it is missing.
  4. Builds NeuralAmpModeler-vst3 (Release|x64) through the .sln.
  5. Assembles dist\ (bundle, user guide as README.md, LICENSES\) and release\DualNAM_A2_Full-<ver>-win-x64.zip.

  Nothing is installed into C:\Program Files unless -Install is given (needs an
  elevated PowerShell).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\build.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\build.ps1 -Install
#>
param(
  [switch]$Install,
  [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Version = '1.2.0'
$SdkTag = 'v3.8.0_build_66'
$Name = 'DualNAM_A2_Full'
# Name of the shipped bundle. Only the folder and file name carry the version;
# the VST3 class ID and the plug-in name shown in the host stay the same.
$BundleName = "Dual NAM A2 Full $Version"

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

# ── 1. Toolchain ──────────────────────────────────────────────────────────────
Step 'Toolchain'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 Build Tools (workload "Desktop development with C++").' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild with the MSVC x64 toolset not found (component Microsoft.VisualStudio.Component.VC.Tools.x86.x64).' }
Write-Host "MSBuild: $msbuild"
$git = (Get-Command git -ErrorAction Stop).Source
Write-Host "Git:     $git"

# ── 2. Submodules ─────────────────────────────────────────────────────────────
Step 'Submodules'
Push-Location $Root
try {
  if (Test-Path (Join-Path $Root '.git')) {
    # Top level only: the nested submodules of NeuralAmpModelerCore/AudioDSPTools
    # are not used by the plug-in build (it uses the top-level eigen/AudioDSPTools).
    & $git submodule update --init
    if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }
    & $git submodule status
  } else {
    # Source archive: the dependencies are fetched by hand (see BUILD.md).
    Write-Host 'Not a git checkout: skipping submodule update.'
  }
  foreach ($dep in 'iPlug2\IPlug', 'NeuralAmpModelerCore\NAM', 'AudioDSPTools\dsp', 'eigen\Eigen') {
    if (-not (Test-Path (Join-Path $Root $dep))) { throw "Dependency missing: $dep (see BUILD.md, section 3)." }
  }
} finally { Pop-Location }

# ── 3. VST3 SDK ───────────────────────────────────────────────────────────────
Step "VST3 SDK ($SdkTag)"
$sdkDir = Join-Path $Root 'iPlug2\Dependencies\IPlug\VST3_SDK'
$sdkTypes = Join-Path $sdkDir 'pluginterfaces\vst\vsttypes.h'
if (-not (Test-Path $sdkTypes)) {
  $bash = Join-Path (Split-Path (Split-Path $git)) 'bin\bash.exe'
  if (-not (Test-Path $bash)) { throw "Git Bash not found at $bash (needed for iPlug2's download-vst3-sdk.sh)." }
  Push-Location (Join-Path $Root 'iPlug2\Dependencies\IPlug')
  try {
    & $bash ./download-vst3-sdk.sh $SdkTag
    if ($LASTEXITCODE -ne 0) { throw 'VST3 SDK download failed' }
  } finally { Pop-Location }
}
$sdkVersion = (Select-String -Path $sdkTypes -Pattern 'kVstVersionString\s+"([^"]+)"').Matches[0].Groups[1].Value
Write-Host "SDK: $sdkVersion"
if ($sdkVersion -ne 'VST 3.8.0') { throw "Unexpected VST3 SDK version '$sdkVersion' (expected VST 3.8.0)." }

# ── 4. Build ──────────────────────────────────────────────────────────────────
Step 'Build NeuralAmpModeler-vst3 Release|x64'
$outVst3 = Join-Path $Root 'out\VST3'
New-Item -ItemType Directory -Force $outVst3 | Out-Null
$target = if ($Clean) { 'NeuralAmpModeler-vst3:Rebuild' } else { 'NeuralAmpModeler-vst3' }
# VST3_64_PATH: the post-build step copies the bundle there. Point it into the
# workspace so a plain build never writes to C:\Program Files.
& $msbuild (Join-Path $Root 'NeuralAmpModeler\NeuralAmpModeler.sln') "/t:$target" /p:Configuration=Release /p:Platform=x64 "/p:VST3_64_PATH=$outVst3" /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
$bundle = Join-Path $Root "NeuralAmpModeler\build-win\$Name.vst3"
$binary = Join-Path $bundle "Contents\x86_64-win\$Name.vst3"
if (-not (Test-Path $binary)) { throw "Bundle binary missing: $binary" }

# ── 5. dist\ and ZIP ──────────────────────────────────────────────────────────
Step 'dist'
$dist = Join-Path $Root 'dist'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
$distBundle = Join-Path $dist "$BundleName.vst3"
Copy-Item -Recurse -Force $bundle $distBundle
# A Windows VST3 bundle must contain Contents\x86_64-win\<bundle name>.vst3.
Rename-Item -LiteralPath (Join-Path $distBundle "Contents\x86_64-win\$Name.vst3") -NewName "$BundleName.vst3"
# iPlug2's bundle script always creates the 32-bit folder; this is a 64-bit-only build.
$x86 = Join-Path $distBundle 'Contents\x86-win'
if ((Test-Path $x86) -and -not (Get-ChildItem $x86 -Force)) { Remove-Item -Force $x86 }
# The repository's README.md is the project page; the user guide shipped in the
# ZIP as README.md is docs\USER_GUIDE.md.
Copy-Item (Join-Path $Root 'docs\USER_GUIDE.md') (Join-Path $dist 'README.md')
Copy-Item (Join-Path $Root 'Install.ps1') (Join-Path $dist 'Install.ps1')
New-Item -ItemType Directory -Force (Join-Path $dist 'docs') | Out-Null
Copy-Item (Join-Path $Root 'docs\screenshot.png') (Join-Path $dist 'docs\screenshot.png')

$lic = Join-Path $dist 'LICENSES'
New-Item -ItemType Directory -Force $lic | Out-Null
$licenseFiles = [ordered]@{
  'LICENSE'                                                  = 'DualNAM_A2_Full-MIT.txt'
  'NeuralAmpModeler\installer\ThirdPartyNotices.txt'         = 'ThirdPartyNotices.txt'
  'NeuralAmpModelerCore\LICENSE'                             = 'NeuralAmpModelerCore-MIT.txt'
  'AudioDSPTools\LICENSE'                                    = 'AudioDSPTools-MIT.txt'
  'iPlug2\LICENSE.txt'                                       = 'iPlug2-WDL-License.txt'
  'iPlug2\Dependencies\IPlug\VST3_SDK\LICENSE.txt'           = 'Steinberg-VST3-SDK-MIT.txt'
  'iPlug2\Dependencies\IGraphics\NanoVG\LICENSE.txt'         = 'NanoVG-zlib.txt'
  'iPlug2\Dependencies\IGraphics\NanoSVG\LICENSE.txt'        = 'NanoSVG-zlib.txt'
  'eigen\COPYING.MPL2'                                       = 'Eigen-MPL-2.0.txt'
  'eigen\COPYING.README'                                     = 'Eigen-COPYING.README.txt'
  'LICENSES\Roboto-Apache-2.0.txt'                           = 'Roboto-Apache-2.0.txt'
  'LICENSES\Michroma-OFL-1.1.txt'                            = 'Michroma-OFL-1.1.txt'
}
foreach ($src in $licenseFiles.Keys) {
  $p = Join-Path $Root $src
  if (-not (Test-Path $p)) { throw "License file missing: $src" }
  Copy-Item $p (Join-Path $lic $licenseFiles[$src])
}

# Inside the bundle: the current notices in Contents\Resources (the post-build
# step only refreshes them when the DLL is relinked) and, in
# Contents\Resources\Licenses, exactly the license texts the notices refer to.
$notices = Join-Path $Root 'NeuralAmpModeler\installer\ThirdPartyNotices.txt'
$bundleRes = Join-Path $distBundle 'Contents\Resources'
Copy-Item -Force $notices (Join-Path $bundleRes 'ThirdPartyNotices.txt')
$bundleLic = Join-Path $bundleRes 'Licenses'
New-Item -ItemType Directory -Force $bundleLic | Out-Null
$referenced = [regex]::Matches((Get-Content -Raw $notices), 'LICENSES/([A-Za-z0-9_.-]+\.txt)') |
  ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
foreach ($f in $referenced) {
  $p = Join-Path $lic $f
  if (-not (Test-Path $p)) { throw "ThirdPartyNotices.txt refers to LICENSES/$f, which is not in the package." }
  Copy-Item $p (Join-Path $bundleLic $f)
}

$release = Join-Path $Root 'release'
New-Item -ItemType Directory -Force $release | Out-Null
$zip = Join-Path $release "$Name-$Version-win-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# Entries are written one by one rather than with Compress-Archive /
# CreateFromDirectory: those skip the hidden desktop.ini / PlugIn.ico (the
# bundle's folder icon) or store '\' as the path separator under .NET Framework,
# which other unzip tools turn into file names containing backslashes.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
  $distFull = (Resolve-Path $dist).Path.TrimEnd('\') + '\'
  Get-ChildItem -Recurse -Force -File $dist | ForEach-Object {
    $entry = $_.FullName.Substring($distFull.Length).Replace('\', '/')
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
  }
} finally { $archive.Dispose() }
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash
Set-Content -Encoding ascii -Path "$zip.sha256" -Value "$hash  $(Split-Path -Leaf $zip)"
Write-Host "dist:    $dist"
Write-Host "zip:     $zip"
Write-Host "sha256:  $hash"

# ── Optional install ──────────────────────────────────────────────────────────
if ($Install) {
  Step 'Install'
  # Same script as in the ZIP: removes older copies (same class ID) first.
  & (Join-Path $dist 'Install.ps1')
}
Write-Host "`nDone." -ForegroundColor Green
