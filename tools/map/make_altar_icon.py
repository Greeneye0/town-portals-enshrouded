"""The loading-screen map's Flame Altar icon: a stone pedestal and bowl with a flame, painted in
the map's illustrated style (dark outline, warm light). assets/map/altar.png (all of it, a
fallback), altar-base.png (the stone) and altar-flame.png (the flame alone, animated in game:
it flickers over the base), each 128x128 RGBA in the same frame.
Run: python make_altar_icon.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, '..', '..', 'assets', 'map')
S = 512  # drawn large, scaled down for smooth edges
INK = (34, 22, 16, 255)


def base():
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # Steps and pedestal.
    d.polygon([(110, 470), (402, 470), (372, 430), (140, 430)], fill=(118, 108, 98, 255), outline=INK, width=10)
    d.polygon([(170, 432), (342, 432), (316, 330), (196, 330)], fill=(146, 134, 120, 255), outline=INK, width=10)
    d.line([(214, 345), (204, 420)], fill=(108, 98, 88, 255), width=8)
    d.line([(298, 345), (308, 420)], fill=(108, 98, 88, 255), width=8)
    # Bowl, with glowing embers on the rim.
    d.chord([120, 230, 392, 360], 0, 180, fill=(96, 82, 70, 255), outline=INK, width=10)
    d.rectangle([120, 286, 392, 300], fill=(70, 58, 50, 255))
    d.ellipse([124, 270, 388, 312], fill=(255, 140, 40, 255), outline=INK, width=10)
    return im


def flame():
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([140, 40, 372, 300], fill=(255, 150, 40, 150))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(40)))
    d = ImageDraw.Draw(im)
    # Flame: outer orange, inner yellow, white core; its foot hidden in the bowl.
    d.polygon([(256, 40), (318, 150), (342, 228), (306, 290), (206, 290), (170, 228), (196, 150)], fill=(255, 122, 30, 255), outline=INK, width=10)
    d.polygon([(258, 110), (296, 190), (300, 250), (256, 286), (212, 250), (218, 190)], fill=(255, 200, 70, 255))
    d.polygon([(256, 175), (276, 230), (256, 280), (236, 230)], fill=(255, 246, 210, 255))
    return im


def main():
    b, f = base(), flame()
    whole = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    whole.alpha_composite(f)
    whole.alpha_composite(b)  # the bowl's rim in front of the flame's foot
    for name, im in (('altar.png', whole), ('altar-base.png', b), ('altar-flame.png', f)):
        im.resize((128, 128), Image.LANCZOS).save(os.path.join(DIR, name))
    print('altar.png, altar-base.png, altar-flame.png written')


if __name__ == '__main__':
    main()
