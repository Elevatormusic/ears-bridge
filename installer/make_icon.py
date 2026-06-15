#!/usr/bin/env python3
"""Generate the EARS Bridge app icon — Apple macOS style.

A system-blue rounded-rectangle ("squircle") tile with a subtle vertical gradient
and a clean white headphones glyph, matching the redesigned UI's system-blue accent
(#0091FF). Rendered at 2x and downsampled for crisp edges. Writes:
  installer/assets/icon.png  (1024, for the JUCE app icon / macOS .icns)
  installer/assets/icon.ico  (multi-resolution, for Inno Setup + Windows shortcuts)

Run:  python installer/make_icon.py   (requires Pillow)
"""
import os
from PIL import Image, ImageDraw

W = 1024
SS = 2                      # supersample factor
TOP = (43, 165, 255)        # lighter system blue (top of gradient)
BOT = (0, 122, 245)         # system blue (bottom)
WHITE = (255, 255, 255, 255)
PAD = 92                    # tile inset within the canvas (macOS-style padding)
RAD = 196                   # squircle corner radius


def vgradient(size, top, bot):
    g = Image.new("RGB", (1, size))
    for y in range(size):
        t = y / (size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return g.resize((size, size))


def headphones(draw, cx, cy, r, color, ss):
    bbox = [int((cx - r) * ss), int((cy - r) * ss), int((cx + r) * ss), int((cy + r) * ss)]
    draw.arc(bbox, start=180, end=360, fill=color, width=int(r * 0.30 * ss))
    ew, eh = r * 0.30, r * 0.58
    for sx in (cx - r * 0.95, cx + r * 0.95 - ew):
        draw.rounded_rectangle(
            [int(sx * ss), int(cy * ss), int((sx + ew) * ss), int((cy + eh) * ss)],
            radius=int(ew * 0.5 * ss), fill=color)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "assets")
    os.makedirs(out, exist_ok=True)

    s = W * SS
    # gradient-filled squircle on a transparent canvas
    grad = vgradient(s, TOP, BOT)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [PAD * SS, PAD * SS, s - PAD * SS, s - PAD * SS], radius=RAD * SS, fill=255)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    # subtle top highlight band (a lighter arc inside the top edge) for depth
    d.rounded_rectangle(
        [PAD * SS, PAD * SS, s - PAD * SS, (PAD + 8) * SS],
        radius=RAD * SS, fill=(255, 255, 255, 38))
    # white headphones glyph, centred
    headphones(d, W * 0.5, W * 0.5, W * 0.30, WHITE, SS)

    icon = img.resize((W, W), Image.LANCZOS)
    icon.save(os.path.join(out, "icon.png"), "PNG")
    icon.save(os.path.join(out, "icon.ico"),
              sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", os.path.join(out, "icon.png"), "and icon.ico")


if __name__ == "__main__":
    main()
