#!/usr/bin/env python3
"""Generate the Marathon-style "How it works" signal-path diagram.

A dark schematic panel: four stage modules (EARS -> EARS Bridge -> virtual cable
-> Dirac Live) wired left to right with labelled arrows. Output:
  assets/how-it-works.png

Run:  python assets/make_diagram.py   (requires Pillow; Windows system fonts)
"""
import os
import math
from PIL import Image, ImageDraw, ImageFont

INK     = (24, 20, 16)
PANEL   = (33, 28, 23)     # lifted ink for module bodies
CREAM   = (236, 229, 211)
ORANGE  = (255, 87, 34)
TEAL    = (40, 210, 188)
MAGENTA = (255, 45, 120)
LIME    = (201, 231, 64)
ON = {ORANGE: CREAM, TEAL: INK, MAGENTA: CREAM, LIME: INK}

W, H = 2400, 840
SS = 2
FONTS = "C:/Windows/Fonts"


def s(v):
    return int(round(v * SS))


def font(path, size, variation=None):
    f = ImageFont.truetype(os.path.join(FONTS, path), s(size))
    if variation:
        try:
            f.set_variation_by_name(variation)
        except Exception:
            pass
    return f


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    img = Image.new("RGB", (W * SS, H * SS), INK)
    d = ImageDraw.Draw(img)

    f_top   = font("consola.ttf", 28)
    f_title = font("bahnschrift.ttf", 50, "Bold Condensed")
    f_body  = font("consola.ttf", 33)
    f_cap   = font("consola.ttf", 26)
    f_arr   = font("consola.ttf", 27)

    def tracked(x, y, text, fnt, fill, tr=2):
        x = s(x)
        for c in text:
            d.text((x, s(y)), c, font=fnt, fill=fill)
            x += int(fnt.getlength(c) + s(tr))

    # frame + corner plus marks
    d.rectangle([s(30), s(30), s(2370), s(810)], outline=CREAM, width=s(3))
    for cx, cy in ((72, 72), (2328, 72), (72, 768), (2328, 768)):
        d.line([s(cx - 15), s(cy), s(cx + 15), s(cy)], fill=CREAM, width=s(3))
        d.line([s(cx), s(cy - 15), s(cx), s(cy + 15)], fill=CREAM, width=s(3))

    tracked(72, 84, "SIGNAL PATH  //  CAPTURE -> CORRECT -> MONO -> DIRAC", f_top, TEAL, tr=3)
    d.line([s(72), s(126), s(2328), s(126)], fill=(70, 62, 52), width=s(2))

    NY, NH, HH = 250, 300, 76     # node y, height, header height

    def node(x, w, title, accent, lines, caption):
        d.rectangle([s(x), s(NY), s(x + w), s(NY + NH)], fill=PANEL, outline=CREAM, width=s(3))
        d.rectangle([s(x), s(NY), s(x + w), s(NY + HH)], fill=accent)
        tracked(x + 22, NY + 16, title, f_title, ON[accent], tr=1)
        ly = NY + HH + 34
        for ln in lines:
            d.text((s(x + 24), s(ly)), ln, font=f_body, fill=CREAM)
            ly += 52
        tracked(x + 4, NY + NH + 26, caption, f_cap, accent, tr=2)

    def arrow(x0, x1, label):
        yc = NY + NH // 2
        head = 34
        d.rectangle([s(x0), s(yc - 11), s(x1 - head), s(yc + 11)], fill=CREAM)
        d.polygon([(s(x1 - head), s(yc - 30)), (s(x1), s(yc)), (s(x1 - head), s(yc + 30))], fill=CREAM)
        lw = f_arr.getlength(label)
        tracked((x0 + x1) / 2 - lw / 2 / SS, yc - 64, label, f_arr, TEAL, tr=1)

    # four modules
    node(72,   470, "EARS / EARS PRO", TEAL,
         ["L CAPSULE  >", "R CAPSULE  >", "2-CH USB IN"], "MIC JIG")
    node(742,  680, "EARS BRIDGE", ORANGE,
         ["PER-EAR CAL FIR", "COMBINE -> MONO", "ASYNC SRC"], "48 . 96 . 192 kHz")
    node(1612, 360, "VIRTUAL CABLE", MAGENTA,
         ["VB-CABLE", "BLACKHOLE"], "LOOPBACK")
    node(2052, 246, "DIRAC", LIME,
         ["LIVE", "REC IN"], "ONE MIC")

    arrow(542, 742, "L + R")
    arrow(1422, 1612, "MONO")
    arrow(1972, 2052, "USB")

    # reticle accent, lower-left
    cx, cy, r = 200, 690, 56
    for i in range(24):
        a = math.radians(i * 15)
        d.line([s(cx + (r + 4) * math.cos(a)), s(cy + (r + 4) * math.sin(a)),
                s(cx + (r + (14 if i % 2 == 0 else 8)) * math.cos(a)),
                s(cy + (r + (14 if i % 2 == 0 else 8)) * math.sin(a))], fill=(70, 62, 52), width=s(2))
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], outline=ORANGE, width=s(7))
    d.ellipse([s(cx - r + 20), s(cy - r + 20), s(cx + r - 20), s(cy + r - 20)], outline=CREAM, width=s(4))
    d.ellipse([s(cx - 9), s(cy - 9), s(cx + 9), s(cy + 9)], fill=MAGENTA)
    tracked(280, 672, "INVERSE CAL  ::  L = R DUAL-MONO BUS", f_cap, CREAM, tr=2)

    img.resize((W, H), Image.LANCZOS).save(os.path.join(here, "how-it-works.png"), "PNG")
    print("wrote", os.path.join(here, "how-it-works.png"))


if __name__ == "__main__":
    main()
