# archwindows

A native Windows shell: top bar, workspaces indicator, and a theme + background picker. This is the **pure-UI first
version**; tiling, hotkeys and app-bar registration come in the Windows-specific session.

- C++20 straight onto Win32/GDI. **No CRT, no frameworks.** Third-party bytes: Omarchy's icon font
  (`omarchy.ttf`, MIT, embedded) for the logo and the Claude mark, and Omarchy's UI font JetBrains Mono
  (`assets/fonts`, OFL), both loaded privately for the shell only.
- Colours follow Omarchy exactly: the palettes are Omarchy's `colors.toml`, and derived colours (dim text, hover/
  selected fills, menu and popup borders, window borders, terminal ANSI table) use the same rules as Omarchy's
  shell and terminal templates. `python tools/palette_report.py` writes `build/palette-report.html`, a contrast
  sheet of every surface for every theme. The full styling reference (sources, rules, mechanisms, limits) is [docs/STYLING.md](docs/STYLING.md).
- ~52 KB exe. Idle: ~1.5 MB private / ~0.2 MB working set after the post-paint trim.
- Ships 23 themes (palettes, previews, backgrounds); see `LICENSE` for attributions.

## Win+Space menu and Options

`Win+Space` opens a type-to-filter menu: **Theme and background**, **Options**, **Quit shell**, then every
Start Menu shortcut (sorted). The menu opens on the focused window's monitor (or the one under the
cursor if nothing is focused), and the first window of an app launched from it is moved onto that
monitor, so with tiling on it is tiled there. Enter runs, Esc closes (Esc on the Options page goes back). Packaged
(Store) apps are not listed yet. The menu is always available, even with tiling off; it takes over
Windows' Win+Space input-language switch (use Alt+Shift for that).

**Options** (saved to `HKCU\Software\ArchWindows\Options`; space or Enter toggles, applied immediately):

| Option | Default | Effect |
|---|---|---|
| Tiling | **off** | dwindle tiling with the Super-key chords below. Off: windows roam free like normal Windows and Windows keeps all of its own Win-key shortcuts |
| Hide title bars | off | strip title bars even when not tiling (tiled windows always lose theirs) |
| Theme window colours | on | border, title bar and frame colours follow the theme |
| Gaps between windows | on | 10 px outer, 5 px inner |
| Workspaces switch on all monitors together | on | `Super+1..0`, `Super+Tab` and the bar's workspace numbers switch every monitor to its own workspace N, so a workspace spans all screens. Off: only the active monitor switches |
| Theme drives Windows dark/light and accent | off | Explorer, Settings and other apps follow the theme's dark/light mode and accent; originals restored on quit |
| Theme drives Chrome / Brave colour | on | as Omarchy does: the theme's browser colour (Omarchy's `chromium.theme`, else a neutral seed: `#1c2027` dark, light grey for light themes) through the per-user `BrowserThemeColor` policy, plus `BrowserColorScheme=device` so light/dark follows Windows (Chrome may show "managed by your organization"; running browsers are told to reload policies, as Omarchy does). Removed when the option is turned off |
| Theme drives terminal colours | off | Windows Terminal (live, via an `archwindows` scheme in settings.json), mintty/Git Bash (`.minttyrc`, new windows) and the classic console (`HKCU\Console`, new windows) use the theme's colours. Originals are backed up and put back when the option is turned off |

## Tiling

Turn on *Tiling* in the Options menu (or run with `--tiling` for one session). Every window of the
current workspace is tiled in a dwindle layout: a new window splits the focused one, side by side when
its box is wider than tall. While tiling is on, the bar shows workspaces 1-5 (up to 10 if used), tiled
windows lose their title bars, and these chords work. They are caught by a small
low-level keyboard hook, because Windows already owns most Win-key combinations and `RegisterHotKey`
cannot take them:

| Keys | Action |
|---|---|
| `Super+Enter` | Windows Terminal with its default profile (classic console if Terminal is missing); always on |
| `Super+Shift+B` | your default browser, new window; always on |
| `Super+Ctrl+T` | theme picker, on the focused monitor; always on |
| `Super+Q` / `Super+W` | close window |
| `Super+Arrows` | focus the window in that direction |
| `Super+Shift+Arrows` | swap with the window in that direction |
| `Super+J` | toggle the split direction of the focused window's pair |
| `Super+T` | float <-> tile the focused window (floating windows keep their title bar and old size) |
| `Super+F` | fullscreen (whole work area) |
| `Super+Alt+F` | full width (keeps gaps) |
| `Super+M` | minimise |
| `Super+1..0` | go to workspace 1-10 |
| `Super+Shift+1..0` | move the focused window to workspace 1-10 |
| `Super+Tab` / `Super+Shift+Tab` | next / previous workspace |
| `Super+-` / `Super+=` | focused window 100 px narrower / wider; add Shift for height |

Dialogs, fixed-size windows, tool windows, always-on-top windows and shell windows are never tiled.
Workspaces work by hiding the windows of other workspaces (`SW_HIDE`); quitting shows everything again, and
`--reset` (or simply starting the shell again) shows anything a crash left hidden.

Multi-monitor (up to 4): every monitor has its own bar and its own workspaces 1-10. By default they
switch together (see Options); with that off, the bar's workspace indicator, `Super+1..0` and
`Super+Tab` act on one monitor only (the focused window's, else the one under the cursor). `Super+Shift+Arrows` past the edge of a monitor moves the window to the next one. When
monitors are plugged, unplugged or rearranged, each keeps its workspaces; windows of a monitor that
is gone move to the same workspace number on the primary. Each bar reserves the top strip of its
monitor as an app bar, so maximised and tiled windows start below it. All bars use the primary
monitor's scale (fonts are shared), so on a monitor with a different scale the bar is a little
larger or smaller.

## Window styling

While running, other apps' windows follow the theme (DWM attributes only; nothing is injected): an
accent-coloured border on the focused window, a muted one on the rest, title bar in the theme's
background/text colours, square corners, dark or light frame. Event-driven (WinEvent hooks), no polling,
no window list kept for this. Elevated (admin) windows are not affected.

Title-bar removal skips apps that draw their own (Chrome/Edge/VS Code/Electron, UWP/WinUI apps, File
Explorer, Firefox, Office - see `StripCaption::excludedClass`); those can still be tiled and coloured.

| Flag | Effect |
|---|---|
| `--tiling` | force tiling on for this run |
| `--no-style` | bar and menu only: no window colours, title bars or tiling |
| `--reset` | restore every window's title bar and colours, unhide anything left hidden, then exit |
| `--menu` / `--picker` | open the menu / theme picker at launch |

Quitting normally (System > Quit shell in the menu) restores all windows.

## Bar and panels

The bar follows Omarchy's default layout. Every panel opens under its icon, on that bar's monitor,
and closes on Esc, on a click elsewhere, or on a second click of its icon.

| Widget | Left click | Other |
|---|---|---|
| Omarchy logo | Super+Space menu on this monitor | right: terminal |
| Workspaces (active one is a dot) | switch | scroll: next/previous |
| Clock (`Saturday 18:29`) | calendar with ISO weeks, month arrows | right: next format |
| Keyboard layout (only with 2+ layouts) | next layout | |
| Claude mark (only when `~/.claude` exists) | **Agents** panel | right: start `claude` in a terminal |
| Bluetooth | Settings > Bluetooth | |
| Network | **Network** panel | |
| Volume | **Audio** panel | right: mute, scroll: volume |
| Display | **Display** panel | |
| Battery % / power | **Power** panel | right: hide/show % |

- **Network** (WLAN API + IP Helper): SSID, Wi-Fi switch, a speed-test link, ping and packet loss to 1.1.1.1, live receive/send
  rates, totals, IP and gateway, DNS provider (DHCP / Cloudflare / Google, applied through one elevated
  `Set-DnsClientServerAddress`, so Windows asks for admin; Custom opens Settings), known networks (click to join) and
  other networks (Windows' own network list asks for the password).
- **Audio** (Core Audio): output and input volume sliders, mute switch, every active output/input device; clicking one
  makes it the default (for all roles).
- **Display** (DisplayConfig): Windows text size (100-225%, the "Make text bigger" setting), the scale steps Windows
  offers for the selected display (the same values as Settings), and the display list (the bar's monitor is marked
  `focused`; click a display to choose which one the scale buttons change).
- **Power**: battery state, Windows power mode (Efficiency / Balanced / Performance) and session buttons:
  lock, sleep, sign out, restart, shut down (the last three ask first).
- **Agents**: Claude Code usage read from its local transcripts (`~/.claude/projects`, or `$CLAUDE_CONFIG_DIR`), as
  Omarchy's collector does: tokens by day for the last week and tokens by model (input + output + cache writes + cache
  reads, one count per reply), the plan name, refresh, and a button that starts `claude`. Nothing is sent anywhere.

The menu itself follows Omarchy's: the prompt (`Go…`) is the input line, submenus end in a chevron, search results
show where they live (`Learn › Keybindings`, `Apps › Inno Setup 6`) with the best match on top, apps show their own
colour icons and Start Menu folder path, and lock / sleep / sign out / restart / shut down are rows under System.

## Logo

The bar's logo button shows the Omarchy mark (U+E900 of Omarchy's `omarchy.ttf`, embedded in the exe and
loaded privately, so nothing is installed). To use your own, set two values under
`HKCU\Software\ArchWindows`:

| Value | Type | Meaning |
|---|---|---|
| `LogoFont` | string | family name of an **installed** font |
| `LogoGlyph` | DWORD | Unicode codepoint of the glyph (e.g. `0xE782`) |

If the font is not installed the default is used. Any logo you choose is your own responsibility
(trademarks, font licences); the project itself ships none.

## Build / run

    build.cmd                         # needs VS 2022 C++ tools; outputs build\archwindows.exe
    build\archwindows.exe             # left-click the logo (or run with --picker) for the theme picker

Assets (`themes\`) are found in `assets\` next to the exe or one level up. The bar itself needs none of them.

| Input | Action |
|---|---|
| Logo click | open theme picker |
| Picker: Up/Down/PgUp/PgDn/Home/End, wheel | choose theme (previews live on the bar) |
| Picker: Left/Right, click preview | cycle preview / backgrounds |
| Picker: Enter or double-click | apply theme, set wallpaper, remember choice (HKCU\Software\ArchWindows) |
| Picker: Esc, or focus lost | revert |
| Clock click | cycle format |
| Workspace click / wheel | switch workspace (shown only while tiling is on) |
| Win+Space | menu (launcher, theme picker, Options) |
| Power icon, right-click | quit |

## Installer

    powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 [-Version 0.1.0] [-Rebuild]

Outputs `build\archwindows-setup-<version>.exe` (needs Inno Setup 6: `winget install JRSoftware.InnoSetup`).
It installs the exe and `assets\` to Program Files, every style under `build\msstyles\` (build them first
with `tools\make_msstyles.py`), and optionally SecureUxTheme 4.0.0 (official MSI, SHA-256 pinned in the build
script; needs a restart). A wizard page picks the style to apply. If SecureUxTheme was installed in the same
run, the style is applied at the first sign-in after restarting. Uninstalling switches back to a stock theme
if an Arch style is active, and leaves SecureUxTheme installed (it has its own entry in Installed apps).

## Policy-based design

Each concern is a small type with a static interface; `main.cpp` is the only place they are wired.

| Policy | Contract | Now | Later |
|---|---|---|---|
| Theme | `count/name/bgCount/palette/find` | `EmbeddedThemes` (generated `.rdata`) | user theme dirs |
| Render | frame ctor/dtor, `fill/frame/text/measure/image` | `GdiRenderer` | Direct2D |
| Image | `acquire/release/decode` | `WicImage` (loaded only while picker is open) | - |
| Wallpaper | `set(path)` | `SpiWallpaper` | - |
| Store | `load/save` | `RegistryStore` | - |
| Workspaces | `visible/count/active/focus` | `TileWorkspaces` (fed by the layout) | - |
| Layout | `track/untrack/relayout/focusDir/swapDir/switchTo/...` | `Dwindle` (binary-split layout) | master, scrolling |
| Window style | `styleable/apply/reset` | `DwmStyle` (DWM frame colours) | - |
| Window frame | `hide/restore` | `StripCaption` | - |
| Launcher | `run(path)` | `ShellLaunch` (Start Menu shortcuts) | Store apps |
| Keys | chord table `kChords` | `LowLevelKeys` (Super chords) | - |
| Bar widget | `slot/width/paint/click[/scroll]` (a `concept`) | Logo, Workspaces, Clock, icons, Battery, Power | tray, panels |

`Bar<Bus, R, Widgets...>` inherits every widget (no vtables, no heap) and walks the pack with fold
expressions. `Bus` is the narrow interface through which bar and picker talk to the shell.

## Where the RAM goes (and doesn't)

- GDI, not D2D/DirectWrite: no extra DLLs, no GPU resources.
- Frames are double-buffered per paint and freed immediately; nothing resident.
- Picker window, preview pixels (`VirtualAlloc`) and WIC/ole32 exist only while it is open.
- Wallpaper is applied by path; we never hold the image.
- The menu's item list and Start Menu scan live in one `VirtualAlloc` arena, freed on close.
- Tiler state is fixed static arrays (~17 KB, touched only when tiling is on); window frame state
  lives in properties on the target windows, not in our memory.
- A 1.5 s idle timer calls `SetProcessWorkingSetSize(-1,-1)` after every burst of activity.
- Palettes are `constexpr` data; no parsing at runtime.

## Disk

`tools/import_themes.py <upstream-checkout> assets` regenerates everything (dev-time
only; needs Python + Pillow). Defaults: 480px preview thumbs (JPEG q70), up to 3 backgrounds per theme at
1920px q60 -> ~8.8 MB total. Smaller: `--no-bg` (previews only), `--max-bg 1`, `--bg-width 1280`.
The upstream sources are 79 MB of 8K WebP.

## Deliberately not done yet

Tray, a bluetooth panel, Wi-Fi password entry and the Wi-Fi QR code, weather, brightness, per-monitor scale for the bars, Store (packaged) apps in the
launcher, per-theme border gradients and rounding, user themes, scratchpad/pop-out/master layouts, and
dragging or mouse-resizing tiled windows.
