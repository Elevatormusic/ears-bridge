#!/usr/bin/env python3
"""Generate the EARS Bridge app icon — polished Apple macOS style.

A true superellipse "squircle" in system blue with a refined gradient, a soft
top sheen, a subtle inner bottom shadow for depth, and a clean white headphones
glyph that lifts off the surface with a soft shadow. Rendered at 2x. Writes:
  installer/assets/icon.png  (1024, for the JUCE app icon / macOS .icns)
  installer/assets/icon.ico  (multi-resolution, for Inno Setup + Windows shortcuts)

Run:  python installer/make_icon.py   (requires Pillow)
"""
import os
import math
from PIL import Image, ImageDraw, ImageFilter, ImageChops

W = 1024
SS = 2
S = W * SS
PAD = int(S * 0.085)        # tile inset
N = 5.0                     # superellipse exponent (macOS-like squircle)
TOP = (60, 176, 255)        # gradient top (lighter blue)
BOT = (0, 120, 234)         # gradient bottom (system blue)
WHITE = (255, 255, 255, 255)


def superellipse_mask():
    cx = cy = S / 2
    a = b = S / 2 - PAD
    pts = []
    for i in range(1200):
        th = 2 * math.pi * i / 1200
        ct, st = math.cos(th), math.sin(th)
        x = cx + a * math.copysign(abs(ct) ** (2 / N), ct)
        y = cy + b * math.copysign(abs(st) ** (2 / N), st)
        pts.append((x, y))
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).polygon(pts, fill=255)
    return m


def vgrad(top, bot):
    g = Image.new("RGB", (1, S))
    for y in range(S):
        t = y / (S - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return g.resize((S, S))


def headphones(draw, cx, cy, r, color):
    draw.arc([cx - r, cy - r, cx + r, cy + r], 180, 360, fill=color, width=int(r * 0.27))
    ew, eh = r * 0.30, r * 0.60
    for sx in (cx - r * 0.96, cx + r * 0.96 - ew):
        draw.rounded_rectangle([sx, cy, sx + ew, cy + eh], radius=int(ew * 0.5), fill=color)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "assets")
    os.makedirs(out, exist_ok=True)

    mask = superellipse_mask()
    base = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    base.paste(vgrad(TOP, BOT).convert("RGBA"), (0, 0), mask)

    # soft top sheen (lit-from-above highlight), clipped to the squircle
    sh = Image.new("L", (S, S), 0)
    ImageDraw.Draw(sh).ellipse([S * 0.08, -S * 0.40, S * 0.92, S * 0.52], fill=66)
    sh = sh.filter(ImageFilter.GaussianBlur(S * 0.06))
    base.paste(Image.new("RGBA", (S, S), (255, 255, 255, 255)), (0, 0), ImageChops.multiply(sh, mask))

    # subtle inner bottom shadow for depth
    bs = Image.new("L", (S, S), 0)
    ImageDraw.Draw(bs).ellipse([-S * 0.1, S * 0.62, S * 1.1, S * 1.45], fill=80)
    bs = bs.filter(ImageFilter.GaussianBlur(S * 0.055))
    base.paste(Image.new("RGBA", (S, S), (0, 38, 86, 255)), (0, 0), ImageChops.multiply(bs, mask))

    # glyph drop shadow (lifts the headphones off the surface), clipped to the squircle
    gs = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    headphones(ImageDraw.Draw(gs), S * 0.5, S * 0.5 + S * 0.014, S * 0.30, (0, 34, 78, 150))
    gs = gs.filter(ImageFilter.GaussianBlur(S * 0.012))
    gsc = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gsc.paste(gs, (0, 0), mask)
    base = Image.alpha_composite(base, gsc)

    # white headphones glyph
    gl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    headphones(ImageDraw.Draw(gl), S * 0.5, S * 0.5, S * 0.30, WHITE)
    base = Image.alpha_composite(base, gl)

    icon = base.resize((W, W), Image.LANCZOS)
    icon.save(os.path.join(out, "icon.png"), "PNG")
    icon.save(os.path.join(out, "icon.ico"),
              sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", os.path.join(out, "icon.png"), "and icon.ico")


if __name__ == "__main__":
    main()
