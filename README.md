# archwindows

A native Windows shell: top bar, workspaces indicator, and a theme + background picker. This is the **pure-UI first
version**; tiling, hotkeys and app-bar registration come in the Windows-specific session.

- C++20 straight onto Win32/GDI. **No CRT, no frameworks, no third-party code.**
- ~28 KB exe. Idle: ~1.3 MB private / ~0.2 MB working set after the post-paint trim.
- Ships 23 themes (palettes, previews, backgrounds); see `LICENSE` for attributions.

## Logo

The bar's logo button shows the system's power glyph (Segoe Fluent Icons, already part of
Windows; nothing is bundled). To use your own, set two values under
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
| Workspace click / wheel | switch (visual only for now) |
| Power icon, right-click | quit |

## Policy-based design

Each concern is a small type with a static interface; `main.cpp` is the only place they are wired.

| Policy | Contract | Now | Later |
|---|---|---|---|
| Theme | `count/name/bgCount/palette/find` | `EmbeddedThemes` (generated `.rdata`) | user theme dirs |
| Render | frame ctor/dtor, `fill/frame/text/measure/image` | `GdiRenderer` | Direct2D |
| Image | `acquire/release/decode` | `WicImage` (loaded only while picker is open) | - |
| Wallpaper | `set(path)` | `SpiWallpaper` | - |
| Store | `load/save` | `RegistryStore` | - |
| Workspaces | `count/active/focus` | `StubWorkspaces` | tiler model |
| Bar widget | `slot/width/paint/click[/scroll]` (a `concept`) | Logo, Workspaces, Clock, icons, Battery, Power | tray, panels |

`Bar<Bus, R, Widgets...>` inherits every widget (no vtables, no heap) and walks the pack with fold
expressions. `Bus` is the narrow interface through which bar and picker talk to the shell.

## Where the RAM goes (and doesn't)

- GDI, not D2D/DirectWrite: no extra DLLs, no GPU resources.
- Frames are double-buffered per paint and freed immediately; nothing resident.
- Picker window, preview pixels (`VirtualAlloc`) and WIC/ole32 exist only while it is open.
- Wallpaper is applied by path; we never hold the image.
- A 1.5 s idle timer calls `SetProcessWorkingSetSize(-1,-1)` after every burst of activity.
- Palettes are `constexpr` data; no parsing at runtime.

## Disk

`tools/import_themes.py <upstream-checkout> assets` regenerates everything (dev-time
only; needs Python + Pillow). Defaults: 480px preview thumbs (JPEG q70), up to 3 backgrounds per theme at
1920px q60 -> ~8.8 MB total. Smaller: `--no-bg` (previews only), `--max-bg 1`, `--bg-width 1280`.
The upstream sources are 79 MB of 8K WebP.

## Deliberately not done yet

Real tiling and workspaces, `RegisterHotKey` bindings, `SHAppBarMessage` work-area reservation
(the bar currently overlaps the top 26px of other windows), tray, network/audio/bluetooth panels,
multi-monitor bars, Linux-specific pieces, user themes, and per-theme border gradients (needed by the tiler).
