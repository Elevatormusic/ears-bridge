#!/usr/bin/env python3
"""Generate installer artwork that matches the redesigned app icon.

Writes:
  installer/assets/wizard-large.bmp   Inno Setup welcome/finish banner (left panel)
  installer/assets/wizard-small.bmp   Inno Setup inner-page header icon
  installer/assets/dmg-background.png  macOS .dmg window background (drag-to-Applications)

Run:  python installer/make_installer_art.py   (requires Pillow)
"""
import os
from PIL import Image, ImageDraw, ImageFont

BLUE_TOP = (43, 165, 255)
BLUE_BOT = (0, 122, 245)
INK = (28, 28, 30)
GRAY = (110, 110, 116)
WHITE = (255, 255, 255)
FONTS = "C:/Windows/Fonts"


def font(name, size):
    for n in (name, "segoeui.bold.ttf", "segoeuib.ttf", "arialbd.ttf"):
        try:
            return ImageFont.truetype(os.path.join(FONTS, n), size)
        except Exception:
            continue
    return ImageFont.load_default()


def vgrad(w, h, top, bot):
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / (h - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return g.resize((w, h))


def headphones(d, cx, cy, r, color):
    d.arc([cx - r, cy - r, cx + r, cy + r], 180, 360, fill=color, width=int(r * 0.30))
    ew, eh = r * 0.30, r * 0.58
    for sx in (cx - r * 0.95, cx + r * 0.95 - ew):
        d.rounded_rectangle([sx, cy, sx + ew, cy + eh], radius=int(ew * 0.5), fill=color)


def squircle_icon(size):
    s = size * 3
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    pad, rad = int(s * 0.09), int(s * 0.19)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, s - pad, s - pad], radius=rad, fill=255)
    img.paste(vgrad(s, s, BLUE_TOP, BLUE_BOT).convert("RGBA"), (0, 0), mask)
    headphones(ImageDraw.Draw(img), s / 2, s / 2, s * 0.30, WHITE)
    return img.resize((size, size), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "assets")
    os.makedirs(out, exist_ok=True)

    # ---- Inno wizard large banner (2x of 164x314) ----
    W, H = 328, 628
    big = vgrad(W, H, BLUE_TOP, BLUE_BOT)
    bd = ImageDraw.Draw(big)
    headphones(bd, W / 2, H * 0.36, W * 0.26, WHITE)
    f_title = font("segoeui.ttf", 40)
    f_sub = font("segoeui.ttf", 19)
    title = "EARS Bridge"
    tw = bd.textlength(title, font=f_title)
    bd.text(((W - tw) / 2, H * 0.56), title, font=f_title, fill=WHITE)
    sub = "Headphone calibration bridge"
    sw = bd.textlength(sub, font=f_sub)
    bd.text(((W - sw) / 2, H * 0.56 + 52), sub, font=f_sub, fill=(225, 240, 255))
    big.save(os.path.join(out, "wizard-large.bmp"))

    # ---- Inno small inner-page icon (on the white modern header) ----
    small = Image.new("RGB", (110, 116), WHITE)
    small.paste(squircle_icon(96), (7, 10), squircle_icon(96))
    small.save(os.path.join(out, "wizard-small.bmp"))

    # ---- macOS .dmg window background ----
    DW, DH = 660, 420
    dmg = Image.new("RGB", (DW, DH), (245, 245, 247))
    dd = ImageDraw.Draw(dmg)
    f_dt = font("segoeui.ttf", 26)
    t = "EARS Bridge"
    dd.text(((DW - dd.textlength(t, font=f_dt)) / 2, 34), t, font=f_dt, fill=INK)
    f_ds = font("segoeui.ttf", 15)
    g = "Drag EARS Bridge into your Applications folder"
    dd.text(((DW - dd.textlength(g, font=f_ds)) / 2, 70), g, font=f_ds, fill=GRAY)
    # arrow from the app icon position (~180,220) to Applications (~480,220)
    ay = 220
    dd.line([250, ay, 410, ay], fill=(170, 170, 176), width=4)
    dd.polygon([(410, ay - 12), (436, ay), (410, ay + 12)], fill=(170, 170, 176))
    dmg.save(os.path.join(out, "dmg-background.png"))

    print("wrote wizard-large.bmp, wizard-small.bmp, dmg-background.png")


if __name__ == "__main__":
    main()
