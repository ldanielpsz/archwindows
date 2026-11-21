#!/usr/bin/env python3
"""Dev-time: build a Windows visual style (.msstyles) + .theme file that follows one shell theme.

  python make_msstyles.py <theme-name> [out_dir]        default out_dir: build/msstyles

How: copy the system Aero style, then recolour it from the theme palette
  * every PNG in the style (buttons, scrollbars, tabs, edit boxes, list headers, ...):
    grays are remapped on a ramp  foreground (black) .. background (white),  and saturated
    pixels (the blue accents) take the theme's accent colour; alpha is untouched.
  * every COLOR property of the compiled style (text, border, fill colours) with the same mapping.
Resources are replaced in the copy with UpdateResource, so the system file is never modified.

The result is UNSIGNED. Windows only loads it after a visual-style patcher (SecureUxTheme,
UltraUXThemePatcher) has been installed; this script neither installs nor needs one.
Windows only. Needs Python 3.11+, Pillow and numpy.
"""
import ctypes, io, os, re, shutil, struct, sys
from ctypes import wintypes as w
import numpy as np
from PIL import Image

SRC = r'C:\Windows\Resources\Themes\aero\aero.msstyles'
THEME_SRC = r'C:\Windows\Resources\Themes\aero.theme'
HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, '..', 'src', 'themes.gen.h')

# ----------------------------------------------------------------- theme palette --
def load_theme(name):
    txt = open(GEN, encoding='utf-8').read()
    m = re.search(r'\{L"%s",\s*(true|false),\s*\d+,\s*\{([^}]*)\}' % re.escape(name), txt)
    if not m:
        sys.exit('unknown theme: ' + name)
    cols = [int(x, 16) for x in m.group(2).split(',')]
    rgb = lambda c: np.array([c & 255, (c >> 8) & 255, (c >> 16) & 255], np.float32) / 255  # COLORREF 0xBBGGRR
    return dict(light=m.group(1) == 'true', bg=rgb(cols[0]), fg=rgb(cols[4]), accent=rgb(cols[8]),
                accent_ref=cols[8])

# ----------------------------------------------------------------------- recolour --
class Remap:
    """dark=True: the asset is from a DarkMode_* class, i.e. already dark-on-light inverted by Microsoft.
    For a dark theme those keep their direction (black -> deep background, white -> foreground);
    the normal (light) assets are inverted (white -> background, black -> foreground)."""
    def __init__(self, t, dark=False):
        self.t = t
        self.dark = dark and not t['light']

    def __call__(self, rgb):  # rgb: (N,3) float 0..1 -> (N,3)
        t = self.t
        Y = rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)
        mx, mn = rgb.max(1), rgb.min(1)
        S = (mx - mn) / np.maximum(mx, 1e-6)
        Yc = Y[:, None]
        if self.dark:
            gray = (t['bg'] * 0.6) * (1 - Yc) + t['fg'] * Yc          # black -> deep background, white -> foreground
        else:
            gray = t['fg'] * (1 - Yc) + t['bg'] * Yc                  # black -> foreground, white -> background
        f = ((Y - 0.5) * 1.4)[:, None]
        tone = np.where(f > 0, t['accent'] * (1 - f) + f, t['accent'] * (1 + f))  # accent, lighter/darker with the pixel
        a = np.clip((S - 0.18) / 0.27, 0, 1)[:, None]
        a = a * a * (3 - 2 * a)
        return np.clip(gray * (1 - a) + tone * a, 0, 1)

def recolor_png(data, remap):
    im = Image.open(io.BytesIO(data)).convert('RGBA')
    a = np.asarray(im, np.float32) / 255
    h, wd = a.shape[:2]
    rgb = remap(a[..., :3].reshape(-1, 3)).reshape(h, wd, 3)
    out = np.dstack([rgb, a[..., 3:]])
    buf = io.BytesIO()
    Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA').save(buf, 'PNG')
    return buf.getvalue()

def class_names(h):
    return [x for x in read(h, 'CMAP', 'CMAP').decode('utf-16le').split(chr(0)) if x]

def is_dark_class(names, cid):
    return 0 <= cid < len(names) and 'DarkMode' in names[cid]

def dark_images(blob, names):
    """Image ids referenced by DarkMode_* classes: [3000..3020 (image properties), 206, class, part, state, imageId, 0, 16, ...]."""
    n = len(blob) // 4
    ints = struct.unpack('<%di' % n, blob[:n * 4])
    dark = set()
    for i in range(n - 8):
        if ints[i + 1] == 206 and 3000 <= ints[i] <= 3020 and is_dark_class(names, ints[i + 2]):
            dark.add(ints[i + 5])
    return dark

def recolor_variant(blob, remap_light, remap_dark, names):
    """COLOR records in the compiled style: [name, 204, part, state, ., ., ., 4, COLORREF, 0] (int32 each)."""
    n = len(blob) // 4
    ints = list(struct.unpack('<%dI' % n, blob[:n * 4]))
    hits = 0
    for i in range(n - 9):
        if ints[i + 1] == 204 and 3000 <= ints[i] <= 4000 and ints[i + 7] == 4:
            v = ints[i + 8]
            rgb24 = v & 0xFFFFFF
            if rgb24 == 0xFF00FF:  # transparency key: must stay exact
                continue
            c = np.array([[rgb24 & 255, (rgb24 >> 8) & 255, (rgb24 >> 16) & 255]], np.float32) / 255
            rm = remap_dark if is_dark_class(names, ints[i + 2]) else remap_light
            r, g, b = (rm(c)[0] * 255 + 0.5).astype(int)
            ints[i + 8] = (v & 0xFF000000) | (int(b) << 16) | (int(g) << 8) | int(r)
            hits += 1
    return struct.pack('<%dI' % n, *ints) + blob[n * 4:], hits

# --------------------------------------------------------------------- resources --
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.LoadLibraryExW.restype = w.HMODULE
k.LoadLibraryExW.argtypes = [w.LPCWSTR, w.HANDLE, w.DWORD]
k.FindResourceExW.restype = w.HANDLE
k.FindResourceExW.argtypes = [w.HMODULE, ctypes.c_void_p, ctypes.c_void_p, w.WORD]
k.SizeofResource.restype = w.DWORD
k.SizeofResource.argtypes = [w.HMODULE, w.HANDLE]
k.LoadResource.restype = w.HANDLE
k.LoadResource.argtypes = [w.HMODULE, w.HANDLE]
k.LockResource.restype = ctypes.c_void_p
k.LockResource.argtypes = [w.HANDLE]
k.BeginUpdateResourceW.restype = w.HANDLE
k.BeginUpdateResourceW.argtypes = [w.LPCWSTR, w.BOOL]
k.UpdateResourceW.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, w.WORD, ctypes.c_void_p, w.DWORD]
k.EndUpdateResourceW.argtypes = [w.HANDLE, w.BOOL]
ENUMTYPE = ctypes.WINFUNCTYPE(w.BOOL, w.HMODULE, ctypes.c_void_p, ctypes.c_void_p)
ENUMNAME = ctypes.WINFUNCTYPE(w.BOOL, w.HMODULE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
k.EnumResourceTypesW.argtypes = [w.HMODULE, ENUMTYPE, ctypes.c_void_p]
k.EnumResourceNamesW.argtypes = [w.HMODULE, ctypes.c_void_p, ENUMNAME, ctypes.c_void_p]

_keep = []
def ident(p):
    return p if (p is None or p < 65536) else ctypes.wstring_at(p)
def ptr(x):  # resource id or name -> pointer-sized value that stays valid
    if isinstance(x, int):
        return x
    b = ctypes.create_unicode_buffer(x)
    _keep.append(b)
    return ctypes.addressof(b)

def list_resources(h, only):
    out = []
    types = []
    @ENUMTYPE
    def et(_h, t, _l):
        types.append(ident(t)); return True
    k.EnumResourceTypesW(h, et, None)
    for t in types:
        if t not in only:
            continue
        names = []
        @ENUMNAME
        def en(_h, _t, n, _l):
            names.append(ident(n)); return True
        k.EnumResourceNamesW(h, ptr(t), en, None)
        out += [(t, n) for n in names]
    return out

def read(h, t, n):
    r = k.FindResourceExW(h, ptr(t), ptr(n), 0)
    return ctypes.string_at(k.LockResource(k.LoadResource(h, r)), k.SizeofResource(h, r)) if r else None

# -------------------------------------------------------------------------- main --
def main():
    name = sys.argv[1]
    out_root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'build', 'msstyles')
    out_dir = os.path.abspath(os.path.join(out_root, name))
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, name + '.msstyles')
    shutil.copyfile(SRC, dst)
    os.chmod(dst, 0o666)
    t = load_theme(name)
    remap, remap_dark = Remap(t), Remap(t, dark=True)

    h = k.LoadLibraryExW(SRC, None, 0x2)  # LOAD_LIBRARY_AS_DATAFILE
    res = list_resources(h, ('IMAGE', 'VARIANT'))
    names = class_names(h)
    dark = dark_images(read(h, 'VARIANT', 'NORMAL'), names)
    upd = k.BeginUpdateResourceW(dst, False)
    if not upd:
        sys.exit('BeginUpdateResource failed: %d' % ctypes.get_last_error())
    imgs = cols = 0
    for typ, nm in res:
        data = read(h, typ, nm)
        if data is None:
            continue
        if typ == 'IMAGE' and data[:4] == b'\x89PNG':
            new = recolor_png(data, remap_dark if nm in dark else remap); imgs += 1
        elif typ == 'VARIANT':
            new, c = recolor_variant(data, remap, remap_dark, names); cols += c
        else:
            continue
        buf = ctypes.create_string_buffer(new, len(new))
        if not k.UpdateResourceW(upd, ptr(typ), ptr(nm), 0, buf, len(new)):
            sys.exit('UpdateResource failed: %d' % ctypes.get_last_error())
    if not k.EndUpdateResourceW(upd, False):
        sys.exit('EndUpdateResource failed: %d' % ctypes.get_last_error())
    print('recoloured %d images (%d from DarkMode_ classes) and %d colour properties -> %s' % (imgs, len(dark), cols, dst))

    # .theme: the stock Aero theme pointed at our style, dark/light mode and accent from the palette.
    txt = open(THEME_SRC, encoding='cp1252').read()
    argb = '0XFF%02X%02X%02X' % (t['accent_ref'] & 255, (t['accent_ref'] >> 8) & 255, (t['accent_ref'] >> 16) & 255)
    mode = 'Light' if t['light'] else 'Dark'
    txt = re.sub(r'^DisplayName=.*$', lambda m: 'DisplayName=Arch ' + name, txt, count=1, flags=re.M)
    txt = re.sub(r'^Path=.*aero\.msstyles\s*$', lambda m: 'Path=' + dst, txt, count=1, flags=re.M | re.I)
    txt = re.sub(r'^ColorizationColor=.*$', 'ColorizationColor=' + argb, txt, flags=re.M)
    txt = re.sub(r'^SystemMode=.*$', 'SystemMode=' + mode, txt, flags=re.M)
    txt = re.sub(r'^AppMode=.*$', 'AppMode=' + mode, txt, flags=re.M)
    th = os.path.join(out_dir, name + '.theme')
    open(th, 'w', encoding='utf-16').write(txt)
    print('theme file ->', th)

if __name__ == '__main__':
    main()
