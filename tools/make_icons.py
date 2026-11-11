#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Render the launcher icon (original design: a play-shaped sail on a dusk disc).

Deterministic; requires Pillow on the development host. Writes icons/<size>/harbour-sailtube.png.
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SIZES = (86, 108, 128, 172)
SS = 8  # supersampling


def render(size):
    n = size * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Disc with a vertical dusk gradient.
    top, bottom = (232, 64, 72), (120, 28, 92)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, n - 1, n - 1), fill=255)
    grad = Image.new("RGBA", (n, n))
    gd = ImageDraw.Draw(grad)
    for y in range(n):
        t = y / (n - 1)
        gd.line([(0, y), (n, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    img.paste(grad, (0, 0), mask)
    # Water line.
    d.rectangle((0, int(n * 0.74), n, n), fill=(0, 0, 0, 0))
    wave = Image.new("L", (n, n), 0)
    ImageDraw.Draw(wave).ellipse((0, 0, n - 1, n - 1), fill=255)
    water = Image.new("RGBA", (n, n), (40, 18, 70, 255))
    lower = Image.new("L", (n, n), 0)
    ImageDraw.Draw(lower).rectangle((0, int(n * 0.74), n, n), fill=255)
    from PIL import ImageChops
    img.paste(water, (0, 0), ImageChops.multiply(wave, lower))
    # Sail: a play triangle leaning on a mast.
    mast_x = int(n * 0.36)
    d.polygon([(mast_x, int(n * 0.20)), (mast_x, int(n * 0.66)), (int(n * 0.76), int(n * 0.50))], fill=(255, 255, 255, 255))
    d.rectangle((mast_x - int(n * 0.025), int(n * 0.18), mast_x, int(n * 0.70)), fill=(255, 255, 255, 255))
    # Hull.
    d.polygon([(int(n * 0.24), int(n * 0.70)), (int(n * 0.80), int(n * 0.70)), (int(n * 0.70), int(n * 0.78)), (int(n * 0.32), int(n * 0.78))],
              fill=(255, 236, 214, 255))
    return img.resize((size, size), Image.LANCZOS)


for s in SIZES:
    out = ROOT / "icons" / ("%dx%d" % (s, s)) / "harbour-sailtube.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    render(s).save(out, optimize=True)
    print(out.relative_to(ROOT))
