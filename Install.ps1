<#
.SYNOPSIS
  Installs or updates Dual NAM A2 Full into C:\Program Files\Common Files\VST3.

.DESCRIPTION
  Every version has the same VST3 class ID, so two copies side by side would be
  the same plug-in twice for the host. The script therefore replaces older
  copies, in this order, and changes nothing if any step before the removal
  fails:

  1. Checks the package: exactly one "Dual NAM A2 Full <version>.vst3" folder
     next to this script, containing Contents\x86_64-win\<same name>.vst3 as a
     64-bit Windows DLL whose version info says "Dual NAM A2 Full" <version>.
  2. Copies the new bundle into the target folder under a temporary name and
     checks the copy.
  3. Moves older copies in the standard VST3 folders aside. Only folders named
     exactly "DualNAM_A2_Full.vst3" (1.0.0 - 1.2.0 test builds) or
     "Dual NAM A2 Full <x.y.z>.vst3" are considered, and only if their DLL
     identifies itself as "Dual NAM A2 Full"; anything else is left alone. If
     one of them cannot be moved (file in use), all moves are undone.
  4. Renames the temporary copy to its final name and deletes the old copies.

  Needs an elevated PowerShell (Run as administrator).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\Install.ps1
#>
param(
  [string]$Target = (Join-Path $env:CommonProgramFiles 'VST3'),
  # VST3 folders searched for older copies: the system-wide and the per-user one.
  [string[]]$CleanFolders = @((Join-Path $env:CommonProgramFiles 'VST3'), (Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'))
)

$ErrorActionPreference = 'Stop'
$ProductName = 'Dual NAM A2 Full'
$BundlePattern = '^Dual NAM A2 Full (\d+\.\d+\.\d+)\.vst3$'

# Returns $null if $dir is a complete Dual NAM A2 Full bundle, otherwise the reason.
function Test-Bundle([string]$dir, [string]$expectVersion) {
  $name = Split-Path -Leaf $dir
  $dll = Join-Path $dir ("Contents\x86_64-win\" + $name)
  if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { return "missing $dll" }
  $file = Get-Item -LiteralPath $dll
  if ($file.Length -lt 1MB) { return "$dll is too small ($($file.Length) bytes)" }
  $head = New-Object byte[] 2
  $fs = [IO.File]::OpenRead($dll)
  try { [void]$fs.Read($head, 0, 2) } finally { $fs.Dispose() }
  if ($head[0] -ne 0x4D -or $head[1] -ne 0x5A) { return "$dll is not a Windows DLL" }
  $info = $file.VersionInfo
  if ($info.ProductName -ne $ProductName) { return "$dll is not $ProductName (product name '$($info.ProductName)')" }
  if ($expectVersion -and $info.FileVersion -ne $expectVersion) {
    return "$dll has version '$($info.FileVersion)', expected $expectVersion"
  }
  if (-not (Test-Path -LiteralPath (Join-Path $dir 'Contents\Resources\ThirdPartyNotices.txt'))) {
    return "missing Contents\Resources\ThirdPartyNotices.txt"
  }
  return $null
}

# ── 1. Check the package before touching anything ────────────────────────────
$bundles = @(Get-ChildItem -LiteralPath $PSScriptRoot -Directory | Where-Object { $_.Name -match $BundlePattern })
if ($bundles.Count -ne 1) {
  throw "Expected exactly one '$ProductName <version>.vst3' folder next to this script, found $($bundles.Count). Nothing was changed."
}
$bundle = $bundles[0]
$version = [regex]::Match($bundle.Name, $BundlePattern).Groups[1].Value
$problem = Test-Bundle $bundle.FullName $version
if ($problem) { throw "The package is incomplete: $problem. Nothing was changed." }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$needsAdmin = @($Target) + $CleanFolders | Where-Object { $_.StartsWith($env:ProgramFiles, [StringComparison]::OrdinalIgnoreCase) }
if ($needsAdmin -and -not $isAdmin) { throw "Installing into $Target needs an elevated PowerShell (Run as administrator). Nothing was changed." }

# ── 2. Copy the new bundle under a temporary name and check the copy ─────────
New-Item -ItemType Directory -Force $Target | Out-Null
$dest = Join-Path $Target $bundle.Name
$staging = Join-Path $Target ('.installing-' + $bundle.Name)
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
try {
  Copy-Item -LiteralPath $bundle.FullName -Destination $staging -Recurse
  $stagedDll = Join-Path $staging ("Contents\x86_64-win\" + $bundle.Name)
  if (-not (Test-Path -LiteralPath $stagedDll)) { throw "copy incomplete: missing DLL" }
  $srcHash = (Get-FileHash -LiteralPath (Join-Path $bundle.FullName ("Contents\x86_64-win\" + $bundle.Name))).Hash
  if ((Get-FileHash -LiteralPath $stagedDll).Hash -ne $srcHash) { throw "copy incomplete: DLL differs from the package" }
} catch {
  if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
  throw "Copying to $Target failed ($($_.Exception.Message)). The existing installation was not changed."
}

# ── 3. Find older copies (only our own bundles) ──────────────────────────────
$old = @()
foreach ($dir in (@($Target) + $CleanFolders | Select-Object -Unique)) {
  if (-not (Test-Path -LiteralPath $dir)) { continue }
  Get-ChildItem -LiteralPath $dir -Directory |
    Where-Object { $_.Name -eq 'DualNAM_A2_Full.vst3' -or $_.Name -match $BundlePattern } |
    ForEach-Object {
      $dll = Join-Path $_.FullName ("Contents\x86_64-win\" + $_.Name)
      $own = (Test-Path -LiteralPath $dll -PathType Leaf) -and ((Get-Item -LiteralPath $dll).VersionInfo.ProductName -eq $ProductName)
      if ($own) { $old += $_ } else { Write-Warning "left alone (not identified as $ProductName): $($_.FullName)" }
    }
}
if ((Test-Path -LiteralPath $dest) -and -not ($old | Where-Object { $_.FullName -eq $dest })) {
  Remove-Item -LiteralPath $staging -Recurse -Force
  throw "$dest exists but is not an installation of $ProductName. Nothing was changed."
}

# Move the old copies aside first. Renaming fails cleanly if a file is in use
# (e.g. a running DAW); then every rename is undone and nothing has changed.
$moved = @()
try {
  foreach ($o in $old) {
    $aside = Join-Path $o.Parent.FullName ('.removing-' + $o.Name)
    if (Test-Path -LiteralPath $aside) { Remove-Item -LiteralPath $aside -Recurse -Force }
    Rename-Item -LiteralPath $o.FullName -NewName (Split-Path -Leaf $aside)
    $moved += [pscustomobject]@{ Original = $o.FullName; Aside = $aside }
  }
} catch {
  foreach ($m in $moved) { Rename-Item -LiteralPath $m.Aside -NewName (Split-Path -Leaf $m.Original) }
  Remove-Item -LiteralPath $staging -Recurse -Force
  throw "An older copy could not be replaced ($($_.Exception.Message)); is a DAW still running? Nothing was changed."
}

# ── 4. Put the new bundle in place, then delete the old copies ───────────────
Rename-Item -LiteralPath $staging -NewName $bundle.Name
foreach ($m in $moved) {
  Write-Host "removed old copy: $($m.Original)"
  Remove-Item -LiteralPath $m.Aside -Recurse -Force
}
Write-Host "installed: $dest"
