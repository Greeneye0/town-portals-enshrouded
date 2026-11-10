"""Nexus images for Town Portals 2.0: the new banner and header, and gallery images of the live
view, in the same callout style as make_nexus_images.py (whose helpers this reuses).

Screenshots from testing on 2026-11-10 are in listing-shots/2026-11-10 (shot-<n>.webp). Every
image is cropped to the scene (no health bar, compass, hotbar or other mods' minimaps) and the
owner name the game draws over a portal is painted out. Run: python make_nexus_images_200.py
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# The helpers (font, load, sky_fill, callout, wordmark, save) from the 1.0 script, without its images.
_source = open(os.path.join(HERE, 'make_nexus_images.py'), encoding='utf-8').read()
exec(_source[:_source.index('os.makedirs(OUT')])
SHOTS = os.path.join(HERE, '..', 'listing-shots', '2026-11-10')
LEAP = os.path.join(SRC, 'portal-leap', 'banner-user-leap.png')
os.makedirs(OUT, exist_ok=True)


def shot(name, erase=()):
    global SRC
    keep, SRC = SRC, SHOTS
    try:
        return load(name, erase)
    finally:
        SRC = keep


def v2_badge(im):
    """A "V2" seal beside the painted title, in the title's colours: a fiery orange disc with a
    dark rim and cream lettering."""
    from PIL import ImageDraw, ImageFilter
    over = Image.new('RGBA', im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    cx, cy, r = 1192, 166, 46
    shadow = Image.new('RGBA', im.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse((cx - r + 6, cy - r + 8, cx + r + 6, cy + r + 8), fill=(0, 0, 0, 150))
    over.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(6)))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(120, 30, 10, 255))
    d.ellipse((cx - r + 5, cy - r + 5, cx + r - 5, cy + r - 5), fill=(235, 110, 25, 255))
    d.ellipse((cx - r + 5, cy - r + 5, cx + r - 5, cy + 3), fill=(250, 170, 45, 255))
    d.ellipse((cx - r + 10, cy - r + 10, cx + r - 10, cy + r - 10), outline=(255, 225, 150, 255), width=2)
    f = font(True, int(r * 0.94))
    w = d.textlength('V2', font=f)
    d.text((cx - w / 2, cy - r * 0.6), 'V2', font=f, fill=(255, 244, 214, 255), stroke_width=4, stroke_fill=(90, 20, 5, 255))
    im = im.convert('RGBA')
    im.alpha_composite(over)
    return im


# Header 1300x372: the banner's top (Nexus covers the lower ~45% with the title and stats).
banner = v2_badge(Image.open(LEAP)).convert('RGB')
crop_h = round(372 * banner.width / 1300)
banner.crop((0, 0, banner.width, crop_h)).resize((1300, 372), Image.LANCZOS).save(os.path.join(OUT, 'header-1300x372.jpg'), quality=92)
print('header-1300x372.jpg (1300, 372)')

# 01 the new banner, as it is (also the mod's thumbnail).
save(v2_badge(Image.open(LEAP)), '01-banner.jpg')

# 08 the live view: from the base, the village at Goldrock Saddle through the portal.
im = shot('shot-4.webp', erase=[(105, 92, 400, 205), (715, 0, 905, 52)]).crop((0, 75, 2000, 1000))
callout(im, 1235, 120, 'The other side, live',
        'not a picture: it moves, and day and night change in it', target=(1060, 330), size=40)
callout(im, 1300, 690, 'Walk round it like a window', 'near things shift as you move, far ones less',
        target=(1150, 640), size=40)
wordmark(im)
save(im, '08-live-view.jpg', 2000)

# 09 swimming in a lake, home seen through the portal: the blacksmith and the altar's flame live,
# and the lake's water line across the portal.
im = sky_fill(shot('shot-3.webp'), (455, 228, 635, 300)).crop((0, 95, 2000, 1245))
callout(im, 1080, 60, 'Home, as it is right now',
        'your blacksmith and your altar fire, live', target=(560, 640), size=40)
callout(im, 1080, 900, 'Water, both sides', 'the lake runs across the portal like glass',
        target=(760, 730), size=40)
save(im, '09-live-home.jpg', 2000)

# 10 from home, the beach the portal stands on, with the creatures that live there still about.
im = shot('shot-16.webp').crop((290, 60, 1520, 745))
callout(im, 30, 30, 'What lives there, live too',
        'enemies by the far end stay and show', target=(555, 378), size=30)
save(im, '10-far-creatures.jpg', 2000)
