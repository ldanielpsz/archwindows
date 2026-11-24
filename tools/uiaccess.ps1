#requires -Version 5.1
#requires -RunAsAdministrator
<#
.SYNOPSIS
  Builds, signs and installs archwindows with uiAccess, so it can tile and drive elevated
  (Run as administrator) windows. Or, with -Remove, goes back to the ordinary build.

.DESCRIPTION
  Windows only lets a normal-integrity program control elevated windows if it is a "uiAccess"
  program: its manifest asks for uiAccess, it is Authenticode-signed by a certificate the machine
  trusts, and it runs from a secure folder (Program Files). This script:

    1. Builds build\archwindows.exe with the uiAccess manifest (build.cmd <version> uiaccess).
    2. Stops the running shell and lets the installed exe undo its window changes (--reset).
    3. Removes any certificate an earlier run left, creates a new self-signed code-signing
       certificate, trusts it in LocalMachine\Root, signs the exe with it, and then DELETES THE
       PRIVATE KEY. Nothing on the machine can sign anything else with that trust afterwards; the
       next run simply makes a new certificate.
    4. Copies the signed exe into Program Files and starts it as the signed-in user.

  Programs the shell starts (menu, Super+Enter, browser) are launched through Explorer, so they
  do not inherit the shell's uiAccess right (src/launch.h).

.PARAMETER Version
  Version stamped into the exe.

.PARAMETER NoBuild
  Sign and install build\archwindows.exe as it is (it must have been built with "uiaccess").

.PARAMETER Remove
  Install an ordinary (non-uiAccess) build and remove the certificate.
#>
param(
    [string]$Version = '0.3.1',
    [switch]$NoBuild,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$built   = Join-Path $root 'build\archwindows.exe'
$dir     = Join-Path $env:ProgramFiles 'archwindows'
$dst     = Join-Path $dir 'archwindows.exe'
$subject = 'CN=archwindows uiAccess (local build signing)'

function Remove-OurCerts {
    foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher', 'Cert:\CurrentUser\My') {
        Get-ChildItem $store | Where-Object Subject -eq $subject | ForEach-Object {
            Write-Host "  removing old certificate $($_.Thumbprint) from $store"
            Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue
            if (Test-Path $_.PSPath) { Remove-Item $_.PSPath }
        }
    }
}

if (-not (Test-Path $dir)) { throw "archwindows is not installed in $dir; run the installer first." }

# 1. build
if (-not $NoBuild) {
    $mode = if ($Remove) { '' } else { 'uiaccess' }
    Write-Host "Building $Version $mode"
    & cmd.exe /c "`"$root\build.cmd`" $Version $mode"
    if ($LASTEXITCODE -ne 0) { throw "build.cmd failed ($LASTEXITCODE)" }
}

# 2. stop the shell; the installed exe undoes its window changes (it still has its trust here)
Write-Host 'Stopping the shell'
Get-Process archwindows -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if (Test-Path $dst) { Start-Process $dst -ArgumentList '--reset' -Wait }

# 3. certificate and signature
Remove-OurCerts
if (-not $Remove) {
    Write-Host 'Creating a one-off signing certificate'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
        -NotAfter (Get-Date).AddYears(10)
    $cer = Join-Path $env:TEMP "archwindows-uiaccess-$($cert.Thumbprint).cer"
    try {
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        $sig = Set-AuthenticodeSignature -FilePath $built -Certificate $cert -HashAlgorithm SHA256
        if ($sig.Status -ne 'Valid') { throw "signing failed: $($sig.Status) $($sig.StatusMessage)" }
    } finally {
        Remove-Item $cer -ErrorAction SilentlyContinue
        # The private key goes now: the trust can verify this exe, but sign nothing else.
        Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
    }
    Write-Host "  signed $built ($($cert.Thumbprint)); private key deleted"
}

# 4. install and start as the signed-in user (Explorer launches it, so it is not elevated)
Copy-Item $built $dst -Force
Write-Host "Installed $dst  v$((Get-Item $dst).VersionInfo.FileVersion)"
& explorer.exe $dst
Start-Sleep -Seconds 3
$p = Get-Process archwindows -ErrorAction SilentlyContinue
if (-not $p) { throw 'The shell did not start. If it is the uiAccess build, check its signature: Get-AuthenticodeSignature "' + $dst + '"' }
Write-Host "Running (pid $($p.Id))$(if (-not $Remove) { ' with uiAccess: elevated windows can now be tiled' })"
