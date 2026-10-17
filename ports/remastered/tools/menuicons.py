"""menuicons.py: the PC and the N64 of Boss Rally's Remaster Options page
(rm_menuopts.cpp), into ports/remastered/brally/src/core/menus/rm_menuicons.c.

  PC   the app's drawing of one, ports/common/assets/retro_pc.svg, rasterised
       (rsvg-convert)
  N64  the console built by hand for Top Gear Rally's options screen
       (handicons.py, n64()), drawn as the PC drawing is drawn: an oblique
       view, the front face square on, depth running up and to the right
       (DEPTH_ANGLE, foreshortened by DEPTH_SCALE: the drawing's case recedes
       at 40 degrees, its top as deep on the screen as a third of its
       depth would be), lit from the
       upper left as the drawing is, flatter than the game's icons, in the
       model's own greys (a real console's), 4x4 samples a pixel

The menus are 16-bit pictures (RGB565) blitted with a colour key, so each
pixel becomes:
  the key       where the picture leaves the lobe showing (alpha under a half)
  its colour    over the lobe's blue (43, 80, 117) by its alpha, so the
                antialiased edge sits on the lobe as it would drawn there

    .venv/bin/python ports/remastered/tools/menuicons.py
"""
import math
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import handicons  # noqa: E402

ROOT = os.path.join(HERE, "..", "..", "..")
SVG = os.path.join(ROOT, "ports", "common", "assets", "retro_pc.svg")
OUT = os.path.join(ROOT, "ports", "remastered", "brally", "src", "core", "menus", "rm_menuicons.c")
LOBE = (43, 80, 117)
KEY = 0xF81F                    # magenta: no pixel of either picture comes out as it
PC_W = 70                       # the PC's width in the menus' pixels
N64_W = 70                      # the console's
DEPTH_ANGLE, DEPTH_SCALE = 40.0, 0.32   # the drawing's oblique: depth up and right, foreshortened
LIGHT = (-0.45, -0.55, 0.70)    # from the upper left, in front
AMBIENT = 0.7
SS = 4                          # samples a pixel, each way


def pc_rgba():
    with tempfile.TemporaryDirectory() as d:
        png = os.path.join(d, "pc.png")
        subprocess.run(["rsvg-convert", "-w", str(PC_W), SVG, "-o", png], check=True)
        return Image.open(png).convert("RGBA")


def n64_rgba():
    m, colours = handicons.n64()
    cn = handicons.corner_normals(m)
    P = np.array(m.v)
    th = math.radians(DEPTH_ANGLE)
    dx, dy = DEPTH_SCALE * math.cos(th), DEPTH_SCALE * math.sin(th)
    # the model: x right, y back (its front toward -y), z up; a point's place on
    # the screen moves right and up with its depth, and the eye looks along
    # the direction that moves nothing, from the front
    eye = np.array((dx, -1.0, dy))
    eye /= np.linalg.norm(eye)
    L = np.array(LIGHT) / np.linalg.norm(LIGHT)
    sx = P[:, 0] + P[:, 1] * dx
    sy = P[:, 2] + P[:, 1] * dy
    dz = P @ eye
    used = sorted({v for t in m.t for v in t[:3]})
    x0, x1, y0, y1 = sx[used].min(), sx[used].max(), sy[used].min(), sy[used].max()
    k = N64_W * SS / (x1 - x0)
    W, H = N64_W * SS, int(math.ceil((y1 - y0) * k)) + 1
    X = (sx - x0) * k
    Y = (y1 - sy) * k
    col = np.zeros((H, W, 3))
    alpha = np.zeros((H, W))
    zb = np.full((H, W), -1e9)
    for t, n3 in zip(m.t, cn):
        a, b, c = t[:3]
        ax, ay, bx, by, cx, cy = X[a], Y[a], X[b], Y[b], X[c], Y[c]
        area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        if area >= 0:           # counter-clockwise from outside: the screen's y runs down
            continue
        i0, i1 = max(int(min(ax, bx, cx)), 0), min(int(max(ax, bx, cx)) + 1, W - 1)
        j0, j1 = max(int(min(ay, by, cy)), 0), min(int(max(ay, by, cy)) + 1, H - 1)
        if i1 < i0 or j1 < j0:
            continue
        gx, gy = np.meshgrid(np.arange(i0, i1 + 1) + 0.5, np.arange(j0, j1 + 1) + 0.5)
        w0 = ((bx - gx) * (cy - gy) - (cx - gx) * (by - gy)) / area
        w1 = ((cx - gx) * (ay - gy) - (ax - gx) * (cy - gy)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        z = w0 * dz[a] + w1 * dz[b] + w2 * dz[c]
        sub = zb[j0:j1 + 1, i0:i1 + 1]
        win = inside & (z > sub)
        if not win.any():
            continue
        n = w0[..., None] * n3[0] + w1[..., None] * n3[1] + w2[..., None] * n3[2]
        n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-12
        shade = AMBIENT + (1 - AMBIENT) * np.clip(n @ L, 0, 1)
        base = np.array(colours[t[3]])
        sub[win] = z[win]
        col[j0:j1 + 1, i0:i1 + 1][win] = (base[None, :] * shade[win][:, None])
        alpha[j0:j1 + 1, i0:i1 + 1][win] = 1.0
    h = H // SS
    col, alpha = col[:h * SS], alpha[:h * SS]
    a = alpha.reshape(h, SS, N64_W, SS).mean(axis=(1, 3))
    c = (col * alpha[..., None]).reshape(h, SS, N64_W, SS, 3).sum(axis=(1, 3))
    c = c / np.maximum(alpha.reshape(h, SS, N64_W, SS).sum(axis=(1, 3)), 1e-9)[..., None]
    # (the model's colours are the screen's, as the game's icons give theirs: handicons.srgb5)
    rgba = np.dstack([np.clip(c * 255 + 0.5, 0, 255), a * 255 + 0.5]).astype(np.uint8)
    return Image.fromarray(rgba, "RGBA")


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def pixels(im):
    w, h = im.size
    px = im.load()
    out = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                out.append(KEY)
                continue
            k = a / 255.0
            v = rgb565(*[round(v * k + l * (1 - k)) for v, l in zip((r, g, b), LOBE)])
            out.append(v if v != KEY else KEY ^ 0x20)
    return w, h, out


def main():
    icons = [("pc", pc_rgba()), ("n64", n64_rgba())]
    if len(sys.argv) > 1:                         # a look at them: menuicons.py DIR
        for name, im in icons:
            im.save(os.path.join(sys.argv[1], "rm_%sicon.png" % name))
    with open(OUT, "w") as f:
        f.write("/* rm_menuicons.c: the PC and the N64 of the Remaster Options page\n"
                " * (rm_menuopts.cpp) in RGB565, the key where the lobe shows.  Written by\n"
                " * ports/remastered/tools/menuicons.py: do not edit. */\n")
        f.write("const unsigned short rm_menuicon_key = 0x%04X;\n" % KEY)
        for name, im in icons:
            w, h, px = pixels(im)
            f.write("const int rm_%sicon_w = %d, rm_%sicon_h = %d;\n" % (name, w, name, h))
            f.write("const unsigned short rm_%sicon[%d] = {\n" % (name, w * h))
            for i in range(0, len(px), 12):
                f.write("    " + ", ".join("0x%04X" % v for v in px[i:i + 12]) + ",\n")
            f.write("};\n")
            print("menuicons: %s %dx%d" % (name, w, h))
    print("menuicons: -> %s" % os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
