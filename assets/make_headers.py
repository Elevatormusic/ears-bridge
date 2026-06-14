#!/usr/bin/env python3
"""Generate Marathon-style section-header strips for the README.

Each section gets a numbered "panel" header: an accent number chip, a condensed
caps title, a baseline rule, and a small measurement-reticle motif. Output:
  assets/headers/NN-slug.png

Run:  python assets/make_headers.py   (requires Pillow; Windows system fonts)
"""
import os
import math
from PIL import Image, ImageDraw, ImageFont

INK     = (24, 20, 16)
CREAM   = (236, 229, 211)
ORANGE  = (255, 87, 34)
TEAL    = (40, 210, 188)
MAGENTA = (255, 45, 120)
LIME    = (201, 231, 64)

# Number-chip text color per accent (dark text on bright chips, cream on saturated).
ON = {ORANGE: CREAM, TEAL: INK, MAGENTA: CREAM, LIME: INK}

W, H = 2400, 200
SS = 2
FONTS = "C:/Windows/Fonts"

# (number, anchor-slug, TITLE, accent)
SECTIONS = [
    (1,  "how-it-works",                 "HOW IT WORKS",        ORANGE),
    (2,  "status-and-platform-support",  "STATUS // PLATFORMS", TEAL),
    (3,  "requirements",                 "REQUIREMENTS",        MAGENTA),
    (4,  "install-windows",              "INSTALL // WINDOWS",  LIME),
    (5,  "install-macos",                "INSTALL // macOS",    ORANGE),
    (6,  "build-from-source-developers", "BUILD FROM SOURCE",   TEAL),
    (7,  "usage",                        "USAGE",               MAGENTA),
    (8,  "calibration-files",            "CALIBRATION FILES",   LIME),
    (9,  "notes-and-tips",               "NOTES & TIPS",        ORANGE),
    (10, "health-indicators",            "HEALTH INDICATORS",   TEAL),
    (11, "building-and-testing",         "BUILDING & TESTING",  MAGENTA),
    (12, "project-structure",            "PROJECT STRUCTURE",   LIME),
    (13, "bench-validation",             "BENCH VALIDATION",    ORANGE),
    (14, "license",                      "LICENSE",             TEAL),
    (15, "acknowledgements",             "ACKNOWLEDGEMENTS",    MAGENTA),
]


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


def reticle(d, cx, cy, r, accent):
    for i in range(24):
        a = math.radians(i * 15)
        d.line([s(cx + (r + 4) * math.cos(a)), s(cy + (r + 4) * math.sin(a)),
                s(cx + (r + (16 if i % 2 == 0 else 9)) * math.cos(a)),
                s(cy + (r + (16 if i % 2 == 0 else 9)) * math.sin(a))], fill=INK, width=s(2))
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], outline=accent, width=s(8))
    d.ellipse([s(cx - r + 22), s(cy - r + 22), s(cx + r - 22), s(cy + r - 22)], outline=INK, width=s(5))
    g = 12
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        d.line([s(cx + dx * g), s(cy + dy * g), s(cx + dx * (r + 14)), s(cy + dy * (r + 14))],
               fill=INK, width=s(3))
    d.ellipse([s(cx - 11), s(cy - 11), s(cx + 11), s(cy + 11)], fill=accent)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "headers")
    os.makedirs(out, exist_ok=True)

    f_num   = font("bahnschrift.ttf", 104, "Bold Condensed")
    f_title = font("bahnschrift.ttf", 104, "Bold Condensed")
    f_tag   = font("consola.ttf", 24)

    for num, slug, title, accent in SECTIONS:
        img = Image.new("RGB", (W * SS, H * SS), CREAM)
        d = ImageDraw.Draw(img)

        # number chip
        d.rectangle([s(36), s(28), s(186), s(178)], fill=accent)
        nd = f"{num:02d}"
        nw = f_num.getlength(nd)
        d.text((s(111) - nw / 2, s(40)), nd, font=f_num, fill=ON[accent])

        # title
        d.text((s(228), s(40)), title, font=f_title, fill=INK)

        # tiny mono tag above the rule, right-aligned-ish
        tag = "// SECTION " + nd
        d.text((s(2040) - f_tag.getlength(tag), s(36)), tag, font=f_tag, fill=INK)

        # baseline rule + end ticks
        d.line([s(36), s(178), s(2364), s(178)], fill=INK, width=s(3))
        d.line([s(36), s(168), s(36), s(178)], fill=INK, width=s(3))
        d.line([s(2364), s(168), s(2364), s(178)], fill=INK, width=s(3))

        # reticle motif (right)
        reticle(d, 2250, 96, 58, accent)

        img.resize((W, H), Image.LANCZOS).save(os.path.join(out, f"{num:02d}-{slug}.png"), "PNG")
    print(f"wrote {len(SECTIONS)} headers to {out}")


if __name__ == "__main__":
    main()
