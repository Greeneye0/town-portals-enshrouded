"""The loading screen's backdrop, in the style of the game's menus: deep navy, softly brushed,
with faint blue-grey ring arcs. assets/map/backdrop.jpg, 1536x768; drawn cropped to the
screen's shape (never stretched) behind the map, which gets a gold frame in game.
Run: python make_backdrop.py
"""
import math
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'assets', 'map', 'backdrop.jpg')
W, H = 1536, 768


def main():
    rng = np.random.default_rng(7)
    y = np.linspace(0, 1, H)[:, None]
    x = np.linspace(0, 1, W)[None, :]
    # Navy, a little lighter toward the top, darker at the corners.
    base = np.stack([40 + 10 * (1 - y) + 0 * x, 40 + 9 * (1 - y) + 0 * x, 66 + 12 * (1 - y) + 0 * x], -1)
    vignette = 1 - 0.35 * (((x - 0.5) * 1.6) ** 2 + ((y - 0.5) * 1.9) ** 2)
    base = base * vignette[..., None]
    # Brushed: horizontal streaks of soft noise.
    streak = rng.normal(0, 1, (H // 4, W // 64))
    streak = np.asarray(Image.fromarray(((streak - streak.min()) / np.ptp(streak) * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC), np.float32) / 255
    base = base * (0.9 + 0.2 * streak[..., None])
    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8))
    # Ring arcs: large faint circles, a few brighter.
    rings = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(rings)
    for i in range(14):
        cx, cy = rng.uniform(-0.2, 1.2) * W, rng.uniform(-0.3, 1.3) * H
        r = rng.uniform(0.25, 0.9) * H
        width = int(rng.choice([2, 2, 3, 4]))
        alpha = int(rng.uniform(35, 90))
        start = rng.uniform(0, 360)
        d.arc([cx - r, cy - r, cx + r, cy + r], start, start + rng.uniform(90, 260), fill=(120, 140, 190, alpha), width=width)
    rings = rings.filter(ImageFilter.GaussianBlur(1.2))
    img = img.convert('RGBA')
    img.alpha_composite(rings)
    img.convert('RGB').save(OUT, quality=88, optimize=True)
    print(f'{os.path.basename(OUT)} {os.path.getsize(OUT) // 1024} KB')


if __name__ == '__main__':
    main()
