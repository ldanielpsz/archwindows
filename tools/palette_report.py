"""Colour review sheet for every embedded theme.

Reads src/themes.gen.h (the exact palettes the exe ships) and mirrors how the shell maps them:
bar (bar.h), panels and menu (ui.h, menu.h), window frames (winstyle.h DwmStyle), Windows Terminal /
console ANSI table (termtheme.h ansi()), selection and cursor. Writes one HTML page with a mock of
each surface per theme plus WCAG contrast ratios, and prints the pairs that fall below threshold.

    python tools/palette_report.py [out.html]

Keep the mappings below in sync with the C++ when they change.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAMES = ["background", "dark_background", "darker_background", "lighter_background", "foreground",
         "dark_foreground", "light_foreground", "bright_foreground", "accent", "selection", "muted", "red",
         "yellow", "orange", "green", "cyan", "blue", "magenta", "brown", "bright_red", "bright_yellow",
         "bright_green", "bright_cyan", "bright_blue", "bright_magenta"]


def load():
    src = (ROOT / "src" / "themes.gen.h").read_text(encoding="utf-8")
    out = []
    for m in re.finditer(r'\{L"([^"]+)",\s*(true|false),\s*\d+,\s*\{([^}]*)\}\}', src):
        vals = [int(v, 16) for v in m.group(3).split(",")]
        # COLORREF 0x00BBGGRR -> (r, g, b)
        cols = {n: (v & 255, (v >> 8) & 255, (v >> 16) & 255) for n, v in zip(NAMES, vals)}
        out.append((m.group(1), m.group(2) == "true", cols))
    return out


def mix(a, b, pct):  # sys::mix: pct percent of b, integer maths like the C++
    return tuple(int(x + (y - x) * pct / 100) for x, y in zip(a, b))


def lum(c):
    def ch(v):
        v /= 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(v) for v in c)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = sorted((lum(a), lum(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def hx(c):
    return "#%02x%02x%02x" % c


def darker14(c):  # Qt.darker(c, 1.4)
    return tuple(v * 10 // 14 for v in c)


def ensure(c, bg, mn, toward):  # sys::ensure
    r, pct = c, 5
    while contrast(r, bg) < mn and pct <= 100:
        r = mix(c, toward, pct)
        pct += 5
    return r


def surfaces(light, c):  # theme.h Palette, mirroring Omarchy's shell.toml.tpl
    bg, fg = c["background"], c["foreground"]
    sel = mix(bg, fg, 18)
    p = {
        "bg": bg, "fg": fg, "bright": fg, "accent": c["accent"],
        "dim": ensure(darker14(fg), bg, 3.0, fg), "faint": mix(bg, fg, 50),
        "hover": mix(bg, fg, 8), "selected": sel, "menu_sel": mix(bg, fg, 8),
        "accent_text": ensure(c["accent"], sel, 3.0, c["bright_foreground"]),
        "line": mix(bg, fg, 16), "menu_border": fg,
        "border_active": c["accent"], "border_inactive": mix(bg, (0x59, 0x59, 0x59), 67),
    }
    # termtheme.h ansi(): Omarchy's alacritty.toml.tpl mapping
    ansi = [bg, c["red"], c["green"], c["yellow"], c["blue"], c["magenta"], c["cyan"], fg,
            c["muted"], c["bright_red"], c["bright_green"], c["bright_yellow"], c["bright_blue"],
            c["bright_magenta"], c["bright_cyan"], c["bright_foreground"]]
    term = {"bg": bg, "fg": fg, "cursor": c["bright_foreground"], "sel": c["selection"], "selfg": c["bright_foreground"], "ansi": ansi}
    return p, term


# (label, foreground, background, minimum) checks per theme
def checks(p, t, light):
    out = [
        ("bar clock / titles", p["bright"], p["bg"], 4.5),
        ("bar icons, body text", p["fg"], p["bg"], 4.5),
        ("bar unused workspace numbers (fg 50%)", p["faint"], p["bg"], 2.0),
        ("bar hover icon (accent text)", p["accent_text"], p["hover"], 3.0),
        ("menu selected label (accent text on fg 8%)", p["accent_text"], p["menu_sel"], 3.0),
        ("menu border (foreground)", p["menu_border"], p["bg"], 3.0),
        ("panel captions (dim)", p["dim"], p["bg"], 3.0),
        ("panel / menu frame (accent)", p["accent"], p["bg"], 3.0),
        ("window border active vs bg", p["border_active"], p["bg"], 3.0),
        ("window border inactive vs bg", p["border_inactive"], p["bg"], 1.3),
        ("caption text inactive (dim)", p["dim"], p["bg"], 3.0),
        ("terminal text", t["fg"], t["bg"], 4.5),
        ("terminal selected text (fg on selection)", t["fg"], t["sel"], 3.0),
        ("terminal selection visible vs bg", t["sel"], t["bg"], 1.25),
        ("terminal cursor", t["cursor"], t["bg"], 3.0),
    ]
    names = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white", "bright black", "bright red",
             "bright green", "bright yellow", "bright blue", "bright magenta", "bright cyan", "bright white"]
    for i, col in enumerate(t["ansi"]):
        # ANSI black is the background itself in Omarchy's mapping
        if i == 0:
            continue
        out.append(("ansi " + names[i], col, t["bg"], 3.0 if i != 8 else 2.2))
    return out


def card(name, light, c):
    p, t = surfaces(light, c)
    rows = checks(p, t, light)
    fails = [(l, contrast(a, b), mn) for l, a, b, mn in rows if contrast(a, b) < mn]
    a = t["ansi"]
    sw = "".join(f'<div class="sw" style="background:{hx(col)}" title="{i}: {hx(col)} {contrast(col, t["bg"]):.1f}:1">'
                 f'<span style="color:{hx(t["bg"])}">{i}</span></div>' for i, col in enumerate(a))
    chk = "".join(
        f'<tr class="{"bad" if contrast(fg, bg) < mn else ""}"><td>{l}</td>'
        f'<td><span class="pair" style="color:{hx(fg)};background:{hx(bg)}">Aa</span></td>'
        f'<td>{contrast(fg, bg):.2f}</td><td>{mn}</td></tr>' for l, fg, bg, mn in rows)
    term = f"""
<div class="term" style="background:{hx(t['bg'])};color:{hx(t['fg'])}">
<div><span style="color:{hx(a[2])}">user@host</span>:<span style="color:{hx(a[4])}">~/src</span>$ ls --color</div>
<div><span style="color:{hx(a[4])}">assets</span>  <span style="color:{hx(a[2])}">build.cmd</span>  <span style="color:{hx(a[6])}">link</span>  README.md  <span style="color:{hx(a[1])}">core.dump</span>  <span style="color:{hx(a[5])}">image.png</span></div>
<div><span style="color:{hx(a[8])}"># a comment / autosuggestion in bright black</span></div>
<div><span style="background:{hx(t['sel'])}">this line is selected (highlighted text)</span></div>
<div><span style="color:{hx(a[3])}">warning:</span> yellow  <span style="color:{hx(a[9])}">error:</span> bright red  <span style="color:{hx(a[11])}">note</span> <span style="color:{hx(a[12])}">bright blue</span> <span style="color:{hx(a[13])}">bright magenta</span> <span style="color:{hx(a[14])}">bright cyan</span></div>
<div>$ <span style="background:{hx(t['cursor'])}">&nbsp;</span></div>
</div>"""
    bar = f"""
<div class="bar" style="background:{hx(p['bg'])};border-bottom:1px solid {hx(mix(p['bg'], p['fg'], 12))}">
<span style="color:{hx(p['bright'])}">&#9635;</span>
<span class="ws"><b style="background:{hx(p['bright'])}"></b><i style="color:{hx(p['faint'])}">2 3 4 5</i></span>
<span style="color:{hx(p['bright'])}">Saturday 18:29</span>
<span style="color:{hx(p['fg'])}">&#10035; &#5871; wifi vol <span style="background:{hx(p['hover'])};color:{hx(p['accent_text'])}">disp</span> 100%</span>
</div>"""
    menu = f"""
<div class="menu" style="background:{hx(p['bg'])};border:2px solid {hx(p['menu_border'])}">
<div style="color:{hx(p['dim'])}">Go&hellip;</div>
<div class="row" style="background:{hx(p['menu_sel'])};outline:1px solid {hx(mix(p['bg'], p['fg'], 25))};color:{hx(p['accent_text'])}">&#9638; Apps <span>&rsaquo;</span></div>
<div class="row" style="color:{hx(p['bright'])}">&#9881; Setup <span style="color:{hx(p['dim'])}">&rsaquo;</span></div>
<div class="row" style="background:{hx(p['hover'])};color:{hx(p['bright'])}">&#9211; System <span style="color:{hx(p['dim'])}">&rsaquo;</span></div>
<div class="cap" style="color:{hx(p['dim'])}">OUTPUT &nbsp; 45%</div>
<div class="slider"><b style="background:{hx(p['fg'])}"></b><i style="background:{hx(mix(p['bg'], p['fg'], 16))}"></i></div>
</div>"""
    wins = f"""
<div class="wins">
<div class="win" style="border:1px solid {hx(p['border_active'])}"><div class="cap2" style="background:{hx(p['bg'])};color:{hx(p['fg'])}">active window</div><div class="body" style="background:{'#ffffff' if light else '#202020'}"></div></div>
<div class="win" style="border:1px solid {hx(p['border_inactive'])}"><div class="cap2" style="background:{hx(p['bg'])};color:{hx(p['dim'])}">inactive window</div><div class="body" style="background:{'#ffffff' if light else '#202020'}"></div></div>
</div>"""
    status = f'<span class="badge bad">{len(fails)} below threshold</span>' if fails else '<span class="badge ok">all pass</span>'
    return f"""
<section class="card" id="{name}">
<h2>{name} <small>{'light' if light else 'dark'}</small> {status}</h2>
<div class="grid">
<div>{bar}{wins}{menu}</div>
<div>{term}<div class="sws">{sw}</div></div>
<div><table>{chk}</table></div>
</div>
</section>""", fails


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "palette-report.html"
    themes = load()
    cards, summary = [], []
    for name, light, c in themes:
        html, fails = card(name, light, c)
        cards.append(html)
        for l, r, mn in fails:
            summary.append((name, l, r, mn))
    rows = "".join(f'<tr><td><a href="#{n}">{n}</a></td><td>{l}</td><td>{r:.2f}</td><td>{mn}</td></tr>'
                   for n, l, r, mn in summary)
    page = f"""<!doctype html><meta charset="utf-8"><title>archwindows colour review</title>
<style>
body{{font:13px/1.4 Consolas,monospace;background:#111;color:#ddd;margin:16px}}
h1{{font-size:18px}} h2{{font-size:15px;margin:0 0 8px}} small{{color:#888}}
.card{{border:1px solid #333;padding:12px;margin:12px 0;border-radius:6px}}
.grid{{display:grid;grid-template-columns:420px 1fr 380px;gap:14px}}
.bar{{display:flex;justify-content:space-between;align-items:center;padding:4px 8px;height:22px}}
.ws b{{display:inline-block;width:8px;height:8px;border-radius:2px;margin-right:6px}}
.menu{{margin-top:10px;padding:8px;width:260px}} .row{{padding:5px 8px;display:flex;justify-content:space-between}}
.cap{{font-size:10px;letter-spacing:1px;font-weight:bold;margin-top:6px}}
.slider{{display:flex;height:4px;margin:6px 0}} .slider b{{flex:45}} .slider i{{flex:55}}
.wins{{display:flex;gap:8px;margin-top:10px}} .win{{width:200px}} .cap2{{padding:3px 6px}} .body{{height:30px}}
.term{{padding:8px;white-space:pre;font-size:12px}}
.sws{{display:flex;flex-wrap:wrap;margin-top:6px}} .sw{{width:24px;height:24px;font-size:9px;display:flex;align-items:end;justify-content:center}}
table{{border-collapse:collapse;font-size:11px}} td{{padding:1px 6px;border-bottom:1px solid #222}}
tr.bad td{{color:#ff8080}} .pair{{padding:0 6px}}
.badge{{font-size:11px;padding:1px 6px;border-radius:3px;margin-left:8px}} .ok{{background:#1d4d2b}} .bad.badge{{background:#5c1d1d}}
</style>
<h1>archwindows colour review &mdash; {len(themes)} themes, {len(summary)} pairs below threshold</h1>
<p>Thresholds: body text 4.5:1, UI text/icons/borders 3:1, inactive borders 1.3:1, selection fill 1.25:1 (WCAG ratios).</p>
<table><tr><td>theme</td><td>pair</td><td>ratio</td><td>min</td></tr>{rows}</table>
{''.join(cards)}"""
    out.write_text(page, encoding="utf-8")
    print(f"wrote {out}")
    for n, l, r, mn in summary:
        print(f"  {n:18} {l:44} {r:5.2f} < {mn}")


if __name__ == "__main__":
    main()
