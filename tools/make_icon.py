#!/usr/bin/env python3
"""Dev-time: render the Omarchy mark (U+E900 of the omarchy.ttf embedded in src/omarchyfont.gen.h)
into src/archwindows.ico, used by the exe (src/archwindows.rc) and the installer (SetupIconFile).

  python tools/make_icon.py

Light mark on a dark rounded square, so it reads on light and dark taskbars alike. Needs Pillow.
"""
import io, os, re
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
GLYPH = ''
BG, FG = (28, 32, 39, 255), (236, 239, 244, 255)   # Omarchy's neutral dark #1c2027, light foreground
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def font_bytes():
    txt = open(os.path.join(SRC, 'omarchyfont.gen.h'), encoding='utf-8').read()
    body = txt[txt.index('{', txt.index('kOmarchyTtf')) + 1: txt.rindex('}')]
    return bytes(int(x) for x in re.findall(r'\d+', body))


def render(px, ttf):
    s = 4 * px                                     # supersample, then downscale
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.22), fill=BG)
    pad = 0.20 if px >= 32 else 0.14               # small sizes: bigger mark
    box = s * (1 - 2 * pad)
    font = ImageFont.truetype(io.BytesIO(ttf), int(box))
    l, t, r, b = d.textbbox((0, 0), GLYPH, font=font)
    k = box / max(r - l, b - t)
    if abs(k - 1) > 0.01:                          # fit the glyph's ink box exactly
        font = ImageFont.truetype(io.BytesIO(ttf), int(box * k))
        l, t, r, b = d.textbbox((0, 0), GLYPH, font=font)
    d.text(((s - (r - l)) / 2 - l, (s - (b - t)) / 2 - t), GLYPH, font=font, fill=FG)
    return img.resize((px, px), Image.LANCZOS)


def main():
    ttf = font_bytes()
    imgs = [render(px, ttf) for px in SIZES]
    out = os.path.join(SRC, 'archwindows.ico')
    imgs[-1].save(out, format='ICO', sizes=[(p, p) for p in SIZES], append_images=imgs[:-1])
    imgs[-1].save(os.path.join(HERE, '..', 'build', 'icon-preview.png'))
    print('wrote', os.path.normpath(out))


if __name__ == '__main__':
    main()
