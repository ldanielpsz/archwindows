#requires -Version 5.1
<#
.SYNOPSIS
  Builds build\archwindows-setup-<Version>.exe: archwindows + assets + bundled visual styles + SecureUxTheme.

.DESCRIPTION
  1. Builds build\archwindows.exe with build.cmd if it is missing, carries another version, or -Rebuild.
  2. Downloads the pinned SecureUxTheme MSIs from the project's GitHub release into
     build\installer\deps and checks them against the SHA-256 digests GitHub publishes for the
     release. A mismatch deletes the file and stops the build.
  3. Stages every style under build\msstyles\<name>\ (made by tools\make_msstyles.py). The .theme
     is rewritten with a placeholder Path= that the installer fills in with the installed location.
  4. Compiles installer\archwindows.iss with Inno Setup 6 (ISCC.exe).

.PARAMETER Version
  Version stamped on the installer and in Apps > Installed apps.

.PARAMETER Rebuild
  Run build.cmd even if build\archwindows.exe already exists.
#>
param(
    [string]$Version = '0.1.0',
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root    = Split-Path -Parent $PSScriptRoot
$stage   = Join-Path $root 'build\installer'
$deps    = Join-Path $stage 'deps'
$styles  = Join-Path $stage 'styles'

# ---- pinned third-party payload ------------------------------------------------------------
# To update: change the tag and copy the new digests from
#   https://api.github.com/repos/namazso/SecureUxTheme/releases/latest  (assets[].digest)
$UxtVersion = '4.0.0'
$UxtBase    = "https://github.com/namazso/SecureUxTheme/releases/download/v$UxtVersion"
$UxtFiles   = @(
    @{ Name = 'SecureUxTheme_x64.msi';   Sha256 = 'af6ab67e0a283a0138b827b6731d5bb1f4ffc59e6d905f1c41567eb3305a9537' },
    @{ Name = 'SecureUxTheme_ARM64.msi'; Sha256 = '2e5cf62249abdb1d9f6c3b81567587bd13c2094a067b2fa753fde9bd31a14dfb' }
)
$UxtLicenseUrl = "https://raw.githubusercontent.com/namazso/SecureUxTheme/v$UxtVersion/COPYING"

# ---- 1. the shell itself -------------------------------------------------------------------
$exe = Join-Path $root 'build\archwindows.exe'
$stamped = if (Test-Path $exe) { (Get-Item $exe).VersionInfo.ProductVersion } else { $null }
if ($Rebuild -or $stamped -ne $Version) {
    Write-Host "Building archwindows.exe $Version..." -ForegroundColor Cyan
    & cmd.exe /c "`"$root\build.cmd`" $Version"
    if ($LASTEXITCODE -ne 0) { throw "build.cmd failed ($LASTEXITCODE)" }
}

# ---- 2. SecureUxTheme ----------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $deps | Out-Null
foreach ($f in $UxtFiles) {
    $dst = Join-Path $deps $f.Name
    if ((Test-Path $dst) -and (Get-FileHash $dst -Algorithm SHA256).Hash -eq $f.Sha256) {
        Write-Host "ok   $($f.Name) (cached)"
        continue
    }
    Write-Host "get  $($f.Name)" -ForegroundColor Cyan
    Invoke-WebRequest -UseBasicParsing -Uri "$UxtBase/$($f.Name)" -OutFile $dst
    $h = (Get-FileHash $dst -Algorithm SHA256).Hash
    if ($h -ne $f.Sha256) {
        Remove-Item $dst -Force
        throw "SHA-256 mismatch for $($f.Name)`n  expected $($f.Sha256)`n  got      $h"
    }
    Write-Host "ok   $($f.Name) sha256 verified" -ForegroundColor Green
}
$license = Join-Path $deps 'SecureUxTheme-COPYING.txt'
if (-not (Test-Path $license)) {
    Invoke-WebRequest -UseBasicParsing -Uri $UxtLicenseUrl -OutFile $license
}

# ---- 3. visual styles ----------------------------------------------------------------------
if (Test-Path $styles) { Remove-Item $styles -Recurse -Force }
New-Item -ItemType Directory -Force -Path $styles | Out-Null
$names = @()
Get-ChildItem (Join-Path $root 'build\msstyles') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    $n  = $_.Name
    $ms = Join-Path $_.FullName "$n.msstyles"
    $th = Join-Path $_.FullName "$n.theme"
    if (-not ((Test-Path $ms) -and (Test-Path $th))) {
        Write-Host "skip $n (needs $n.msstyles and $n.theme)" -ForegroundColor Yellow
        return
    }
    if ($n -notmatch '^[A-Za-z0-9._-]+$') { throw "style name '$n' must be [A-Za-z0-9._-]" }
    $out = New-Item -ItemType Directory -Force -Path (Join-Path $styles $n)
    Copy-Item $ms $out.FullName
    # make_msstyles.py writes UTF-16 with Path= pointing into build\. Stage it as ANSI (what the
    # stock .theme files use) with a placeholder the installer replaces with {app}\msstyles\...
    (Get-Content $th -Raw -Encoding Unicode) -replace '(?mi)^Path=.*\.msstyles\s*$', 'Path=__ARCH_MSSTYLES__' |
        Set-Content (Join-Path $out.FullName "$n.theme") -Encoding Default -NoNewline
    $names += $n
    Write-Host "ok   style $n"
}
if (-not $names) { Write-Host 'No visual styles staged (build\msstyles is empty) - installer will ship without any.' -ForegroundColor Yellow }

# ---- 4. compile ----------------------------------------------------------------------------
$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw 'Inno Setup 6 not found. Install it with:  winget install --id JRSoftware.InnoSetup -e' }

& $iscc /Q "/DAppVersion=$Version" "/DUxtVersion=$UxtVersion" "/DStyles=$($names -join ';')" (Join-Path $PSScriptRoot 'archwindows.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
$setup = Join-Path $root "build\archwindows-setup-$Version.exe"
Write-Host ("built {0}  {1:N0} bytes" -f $setup, (Get-Item $setup).Length) -ForegroundColor Green
