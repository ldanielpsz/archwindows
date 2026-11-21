#requires -Version 5.1
<#
.SYNOPSIS
  Reverses what Install-ArchTheme.ps1 did: nothing more.

.DESCRIPTION
  1. Switches the active theme back to a stock, signed Windows theme (dark.theme, or aero.theme
     if the install manifest shows the system was in light mode), so you are never left on a
     style whose files are about to be deleted.
  2. Deletes %LOCALAPPDATA%\ArchWindows\themes\ (our copied .msstyles/.theme files and manifest).
  3. Leaves the style patcher itself (SecureUxTheme/UltraUXThemePatcher) installed - it isn't
     ours, other styles may use it, and removing it is a separate, deliberate choice. This
     script prints how to find its own uninstaller if you want that too.

.PARAMETER RemovePatcher
  Also look for a detected patcher's own uninstaller and offer to run it (never run silently).
#>
param([switch]$RemovePatcher)
$ErrorActionPreference = 'Stop'

function Assert-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "Relaunching elevated..." -ForegroundColor Yellow
        $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath)
        if ($RemovePatcher) { $args += '-RemovePatcher' }
        Start-Process powershell -Verb RunAs -ArgumentList $args
        exit
    }
}
Assert-Admin

$installRoot  = Join-Path $env:LOCALAPPDATA 'ArchWindows\themes'
$manifestPath = Join-Path $installRoot 'install-manifest.json'

# ---- 1. revert the active theme to a stock, signed one -------------------------------------
$stockTheme = 'C:\Windows\Resources\Themes\dark.theme'
if (Test-Path $manifestPath) {
    $m = Get-Content $manifestPath -Raw | ConvertFrom-Json
    Write-Host "Reverting theme '$($m.theme)' installed $($m.installedAt)" -ForegroundColor Cyan
} else {
    Write-Host "No install manifest found under $installRoot - reverting the active theme anyway." -ForegroundColor Yellow
}
$light = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' `
          -ErrorAction SilentlyContinue).AppsUseLightTheme
if ($light -eq 1) { $stockTheme = 'C:\Windows\Resources\Themes\aero.theme' }
if (Test-Path $stockTheme) {
    Start-Process $stockTheme
    Start-Sleep -Milliseconds 500   # let Explorer finish applying before its files might go away
} else {
    Write-Host "Stock theme not found at $stockTheme - apply any built-in theme manually before continuing." -ForegroundColor Yellow
}

# ---- 2. remove only what we deployed --------------------------------------------------------
if (Test-Path $installRoot) {
    Remove-Item $installRoot -Recurse -Force
    Write-Host "Removed $installRoot" -ForegroundColor Green
} else {
    Write-Host "Nothing to remove at $installRoot" -ForegroundColor Green
}

# ---- 3. the patcher is not ours: never touched automatically --------------------------------
function Find-PatcherUninstaller {
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($r in $roots) {
        Get-ChildItem $r -ErrorAction SilentlyContinue | ForEach-Object {
            $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($p.DisplayName -match 'SecureUxTheme|UltraUXThemePatcher|UXThemePatcher') {
                [pscustomobject]@{ Name = $p.DisplayName; Cmd = $p.UninstallString }
            }
        }
    }
}
$patcher = Find-PatcherUninstaller
if ($patcher) {
    if ($RemovePatcher) {
        Write-Host "Found $($patcher.Name). Its own uninstaller: $($patcher.Cmd)" -ForegroundColor Cyan
        $ans = Read-Host "Run it now? [y/N]"
        if ($ans -match '^[Yy]') { Start-Process cmd -ArgumentList '/c', $patcher.Cmd -Wait }
    } else {
        Write-Host "$($patcher.Name) is still installed (left alone). Pass -RemovePatcher to be offered its own uninstaller." -ForegroundColor DarkGray
    }
}
Write-Host "Done." -ForegroundColor Green
