#requires -Version 5.1
<#
.SYNOPSIS
  Installs an archwindows-generated visual style (.msstyles) as the active Windows theme.

.DESCRIPTION
  This script only handles OUR part of the chain:
    1. Copies build\msstyles\<Theme>\ (made by tools\make_msstyles.py) to a permanent,
       per-user location: %LOCALAPPDATA%\ArchWindows\themes\<Theme>\
    2. Rewrites the .theme file's Path= to point at that permanent copy.
    3. Applies the theme and writes an install manifest (for the uninstaller).

  It does NOT download, install, or patch anything related to the Windows signature check on
  visual styles. Unsigned styles are rejected by Windows unless a separate, independent tool
  (e.g. SecureUxTheme, https://github.com/namazso/SecureUxTheme) is already installed - that is
  a decision and a download you make yourself, from the project's own release page, because it
  hooks system processes and you should be the one to fetch and trust that binary.

  Run this AFTER SecureUxTheme (or an equivalent) is installed and working with a normal
  recoloured Aero copy. This script will tell you, and stop, if it can't detect a patcher.

.PARAMETER Theme
  Theme name as it appears under build\msstyles (e.g. "retro-82"). Must already be built with
  tools\make_msstyles.py.

.PARAMETER Force
  Skip the SecureUxTheme detection check (use if you know it's installed under a path this
  script doesn't recognise).
#>
param(
    [Parameter(Mandatory)] [string]$Theme,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'

function Assert-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "Relaunching elevated..." -ForegroundColor Yellow
        $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-Theme',$Theme)
        if ($Force) { $args += '-Force' }
        Start-Process powershell -Verb RunAs -ArgumentList $args
        exit
    }
}

# A patcher is detected by its own uninstall registration, never by us touching its files.
function Find-StylePatcher {
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($r in $roots) {
        Get-ChildItem $r -ErrorAction SilentlyContinue | ForEach-Object {
            $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($p.DisplayName -match 'SecureUxTheme|UltraUXThemePatcher|UXThemePatcher') { return $p.DisplayName }
        }
    }
    $null
}

Assert-Admin

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tools\theme-install -> tools -> repo root
$srcDir   = Join-Path $repoRoot "build\msstyles\$Theme"
$stylePath = Join-Path $srcDir "$Theme.msstyles"
$themePath = Join-Path $srcDir "$Theme.theme"
if (-not (Test-Path $stylePath) -or -not (Test-Path $themePath)) {
    throw "Not found: $stylePath / $themePath`nBuild it first: python tools\make_msstyles.py $Theme"
}

if (-not $Force) {
    $patcher = Find-StylePatcher
    if (-not $patcher) {
        Write-Host @"
No visual-style patcher was detected (checked the Windows uninstall registry for
SecureUxTheme / UltraUXThemePatcher).

Windows refuses to load an unsigned .msstyles without one. This script will not download or
install a patcher for you - that's a separate, trust decision you make yourself:

  https://github.com/namazso/SecureUxTheme/releases

Install it, apply its own default/Aero-copy theme once to confirm it's working, then re-run:

  .\Install-ArchTheme.ps1 -Theme $Theme
"@ -ForegroundColor Yellow
        exit 1
    }
    Write-Host "Detected patcher: $patcher" -ForegroundColor Green
}

# ---- our part: deploy + apply + record, so Uninstall-ArchTheme.ps1 can cleanly reverse it ----
$installRoot = Join-Path $env:LOCALAPPDATA 'ArchWindows\themes'
$destDir     = Join-Path $installRoot $Theme
New-Item -ItemType Directory -Force -Path $destDir | Out-Null

Copy-Item $stylePath (Join-Path $destDir "$Theme.msstyles") -Force
$destStyle = Join-Path $destDir "$Theme.msstyles"
$destTheme = Join-Path $destDir "$Theme.theme"

# Rewrite Path= to the permanent copy (the .theme as built points at build\msstyles\...).
(Get-Content $themePath -Raw -Encoding Unicode) `
    -replace '(?m)^Path=.*$', "Path=$destStyle" |
    Set-Content $destTheme -Encoding Unicode

# Record what we did, before touching the active theme, so a failed apply still leaves an
# uninstall path.
$manifestPath = Join-Path $installRoot 'install-manifest.json'
$prevThemeReg = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes' -ErrorAction SilentlyContinue).CurrentTheme
@{
    theme       = $Theme
    installedAt = (Get-Date).ToString('o')
    destDir     = $destDir
    previousTheme = $prevThemeReg
} | ConvertTo-Json | Set-Content $manifestPath -Encoding UTF8

# Apply it the same way Explorer does when you double-click a .theme file.
Start-Process $destTheme
Write-Host "Applied '$Theme'. If Explorer/Settings don't refresh immediately, sign out and back in." -ForegroundColor Green
Write-Host "To undo: .\Uninstall-ArchTheme.ps1" -ForegroundColor Cyan
