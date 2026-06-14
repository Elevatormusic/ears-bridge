#!/usr/bin/env python3
"""Generate the EARS Bridge app/installer icon.

Draws a minimalist headphones glyph (the two-ear EARS jig, "bridged" by the band)
on a dark rounded tile that matches the app's dark theme, and writes:
  installer/assets/icon.png  (512x512, for the JUCE app icon)
  installer/assets/icon.ico  (multi-resolution, for Inno Setup + Windows shortcuts)

Run:  python installer/make_icon.py   (requires Pillow)
"""
import os
from PIL import Image, ImageDraw

BG       = (17, 20, 24, 255)     # dark slate tile
ACCENT   = (45, 212, 191, 255)   # teal — the headphones
SIZE     = 512
RADIUS   = 96                    # tile corner radius


def rounded_tile(size, radius, color):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=color)
    return img


def draw_headphones(img):
    d = ImageDraw.Draw(img)
    # Headband: top arc of an ellipse spanning the "head".
    bbox = [140, 150, 372, 382]
    d.arc(bbox, start=180, end=360, fill=ACCENT, width=34)
    # Earcups hang from the band ends (leftmost/rightmost of the ellipse, at its vertical centre).
    cy0, cy1 = 262, 372
    d.rounded_rectangle([120, cy0, 176, cy1], radius=22, fill=ACCENT)   # left ear
    d.rounded_rectangle([336, cy0, 392, cy1], radius=22, fill=ACCENT)   # right ear
    return img


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "assets")
    os.makedirs(out, exist_ok=True)

    icon = draw_headphones(rounded_tile(SIZE, RADIUS, BG))

    png_path = os.path.join(out, "icon.png")
    icon.save(png_path, "PNG")

    ico_path = os.path.join(out, "icon.ico")
    icon.save(ico_path, sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                               (64, 64), (128, 128), (256, 256)])
    print("wrote", png_path)
    print("wrote", ico_path)


if __name__ == "__main__":
    main()
