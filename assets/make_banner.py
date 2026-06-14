#!/usr/bin/env python3
"""Generate the EARS Bridge README hero banner.

Marathon-inspired graphic style: flat color blocking (cream / warm ink), a big
condensed grotesk logotype, a measurement reticle, and technical readout labels.
Rendered at 2x and downsampled for crisp edges. Output: assets/banner.png

Run:  python assets/make_banner.py   (requires Pillow; uses Windows system fonts)
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageChops

# ---- palette ----
INK     = (24, 20, 16)
CREAM   = (236, 229, 211)
ORANGE  = (255, 87, 34)
TEAL    = (40, 210, 188)
MAGENTA = (255, 45, 120)
LIME    = (201, 231, 64)

W, H = 2400, 760
SS = 2
FONTS = "C:/Windows/Fonts"


def s(v):
    return int(round(v * SS))


def disp(path, size, variation=None):
    f = ImageFont.truetype(os.path.join(FONTS, path), s(size))
    if variation:
        try:
            f.set_variation_by_name(variation)
        except Exception:
            pass
    return f


def tracked(draw, xy, text, font, fill, tracking=0, anchor_left=True):
    """Draw text with manual letter-spacing (Pillow has none). tracking in final px."""
    x, y = s(xy[0]), s(xy[1])
    tr = s(tracking)
    if not anchor_left:
        total = sum(font.getlength(c) + tr for c in text) - tr
        x -= int(total)
    for c in text:
        draw.text((x, y), c, font=font, fill=fill)
        x += int(font.getlength(c) + tr)


def headphones(draw, cx, cy, r, color):
    """Mini headphones glyph centered at (cx,cy), band radius r (final px)."""
    bbox = [s(cx - r), s(cy - r), s(cx + r), s(cy + r)]
    draw.arc(bbox, start=180, end=360, fill=color, width=s(r * 0.34))
    ew, eh = r * 0.30, r * 0.55
    for sx in (cx - r * 0.92, cx + r * 0.92 - ew):
        draw.rounded_rectangle([s(sx), s(cy), s(sx + ew), s(cy + eh)],
                               radius=s(ew * 0.5), fill=color)


def main():
    here = os.path.dirname(os.path.abspath(__file__))

    img = Image.new("RGB", (W * SS, H * SS), CREAM)
    d = ImageDraw.Draw(img)

    # ---- fonts ----
    f_logo  = disp("bahnschrift.ttf", 232, "Bold Condensed")
    f_logo2 = disp("bahnschrift.ttf", 232, "Bold Condensed")
    f_kick  = disp("consola.ttf", 25)
    f_sub   = disp("consola.ttf", 30)
    f_small = disp("consola.ttf", 23)
    f_chip  = disp("consola.ttf", 29)

    PANEL_X = 1512                       # cream | ink split
    FRAME = (30, 30, 2370, 730)

    # ---- right ink panel (color blocking) ----
    d.rectangle([s(PANEL_X), s(FRAME[1]), s(FRAME[2]), s(FRAME[3])], fill=INK)
    d.rectangle([s(PANEL_X), s(FRAME[1]), s(PANEL_X + 7), s(FRAME[3])], fill=ORANGE)  # seam

    # ---- outer technical frame ----
    d.rectangle([s(FRAME[0]), s(FRAME[1]), s(FRAME[2]), s(FRAME[3])], outline=INK, width=s(3))

    # ---- corner registration plus-marks ----
    def plus(cx, cy, col, n=16):
        d.line([s(cx - n), s(cy), s(cx + n), s(cy)], fill=col, width=s(3))
        d.line([s(cx), s(cy - n), s(cx), s(cy + n)], fill=col, width=s(3))
    plus(70, 70, INK); plus(PANEL_X - 40, 70, INK)
    plus(70, 690, INK); plus(PANEL_X - 40, 690, INK)
    plus(PANEL_X + 48, 70, CREAM); plus(2330, 70, CREAM)
    plus(PANEL_X + 48, 690, CREAM); plus(2330, 690, CREAM)

    # ---- left: brand mark + kicker ----
    headphones(d, 118, 132, 30, TEAL_ROUNDEL := TEAL)
    tracked(d, (172, 120), "HEADPHONE  MEASUREMENT  BRIDGE", f_kick, INK, tracking=2)
    d.line([s(172), s(158), s(905), s(158)], fill=INK, width=s(2))

    # ---- left: logotype (stacked, condensed) ----
    d.text((s(96), s(196)), "EARS",   font=f_logo,  fill=INK)
    d.text((s(96), s(404)), "BRIDGE", font=f_logo2, fill=INK)

    # ---- left: color tick bar + sub-label ----
    bx, by = 100, 648
    for i, col in enumerate((ORANGE, TEAL, MAGENTA)):
        d.rectangle([s(bx + i * 116), s(by), s(bx + i * 116 + 96), s(by + 16)], fill=col)
    tracked(d, (100, 686), "miniDSP EARS  ->  DIRAC LIVE", f_sub, INK, tracking=2)

    # ---- right panel: reticle (clipped to the panel) ----
    cx, cy, R = 2070, 384, 252
    ret = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ret)

    def ring(rad, col, w):
        rd.ellipse([s(cx - rad), s(cy - rad), s(cx + rad), s(cy + rad)], outline=col, width=s(w))

    import math
    for i in range(48):                                    # tick ring
        a = math.radians(i * 7.5)
        r0, r1 = R + 6, R + (24 if i % 4 == 0 else 14)
        rd.line([s(cx + r0 * math.cos(a)), s(cy + r0 * math.sin(a)),
                 s(cx + r1 * math.cos(a)), s(cy + r1 * math.sin(a))], fill=CREAM, width=s(2))
    ring(R, TEAL, 12)
    ring(R - 64, CREAM, 7)
    ring(R - 132, ORANGE, 11)
    gap = 30                                               # crosshair with center gap
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        rd.line([s(cx + dx * gap), s(cy + dy * gap),
                 s(cx + dx * (R + 36)), s(cy + dy * (R + 36))], fill=CREAM, width=s(3))
    rd.ellipse([s(cx - 26), s(cy - 26), s(cx + 26), s(cy + 26)], fill=MAGENTA)

    mask = Image.new("L", (W * SS, H * SS), 0)
    ImageDraw.Draw(mask).rectangle([s(PANEL_X + 8), s(FRAME[1]), s(FRAME[2]), s(FRAME[3])], fill=255)
    img.paste(ret, (0, 0), ImageChops.multiply(ret.getchannel("A"), mask))

    # ---- right panel: labels ----
    # version chip
    cw = int(f_chip.getlength("v0.1.1")) + s(44)
    d.rounded_rectangle([s(2196) - cw, s(96), s(2196), s(150)], radius=s(8), outline=CREAM, width=s(2))
    tracked(d, (2196 - 22, 104), "v0.1.1", f_chip, CREAM, tracking=1, anchor_left=False)
    tracked(d, (1604, 110), "RUNFILE", f_small, TEAL, tracking=3)
    tracked(d, (1604, 648), "48 . 96 . 192 kHz", f_small, CREAM, tracking=2)
    tracked(d, (1604, 686), "CAL: L + R  ->  MONO BUS", f_small, TEAL, tracking=2)

    # barcode strip (bottom-right)
    bxc = 2040
    import itertools
    widths = itertools.cycle((3, 7, 4, 11, 5, 3, 8))
    for w in widths:
        if bxc > 2330:
            break
        d.rectangle([s(bxc), s(648), s(bxc + w), s(700)], fill=CREAM)
        bxc += w + 6

    out = os.path.join(here, "banner.png")
    img.resize((W, H), Image.LANCZOS).save(out, "PNG")
    print("wrote", out)


if __name__ == "__main__":
    main()
