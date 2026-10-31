"""Nexus images for Town Portals 1.0.0: callouts in the Embervale Teleport style.

Sources in dist/nexus/source (screenshots from testing, the two banners); output in
dist/nexus/images. Every screenshot is cropped to the scene (no health bar, compass, hotbar,
quest tracker or minimap), and the owner name the game draws over a portal is painted out:
listing images never show a player's name. Run: python make_nexus_images.py
"""
import math
import os
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'dist', 'nexus', 'source')
OUT = os.path.join(HERE, '..', 'dist', 'nexus', 'images')
GOLD = (232, 200, 130)
HOT = (255, 214, 140)
TXT = (230, 220, 200)
BOX = (22, 18, 14, 215)


def font(bold, size):
    return ImageFont.truetype(f"C:/Windows/Fonts/{'georgiab' if bold else 'georgia'}.ttf", size)


def load(name, erase=()):
    """A screenshot, with each erase box (x0, y0, x1, y1) painted out from its surroundings."""
    im = cv2.cvtColor(cv2.imread(os.path.join(SRC, name)), cv2.COLOR_BGR2RGB)
    mask = np.zeros(im.shape[:2], np.uint8)
    for x0, y0, x1, y1 in erase:
        mask[y0:y1, x0:x1] = 255
    if mask.any():
        im = cv2.inpaint(im, mask, 9, cv2.INPAINT_TELEA)
    out = Image.fromarray(im)
    out = ImageEnhance.Contrast(out).enhance(1.05)
    return ImageEnhance.Color(out).enhance(1.05).convert('RGBA')



def sky_fill(im, box, pad=6):
    """A name over plain sky, replaced by the sky beside it: each row of the box takes the
    colours just left and right of it, blended across (sky is a smooth gradient)."""
    a = np.asarray(im).astype(np.float32).copy()
    x0, y0, x1, y1 = box
    left = a[y0:y1, x0 - pad:x0].mean(axis=1)
    right = a[y0:y1, x1:x1 + pad].mean(axis=1)
    t = np.linspace(0, 1, x1 - x0)[None, :, None]
    a[y0:y1, x0:x1] = left[:, None] * (1 - t) + right[:, None] * t
    return Image.fromarray(a.round().astype(np.uint8))

def callout(im, x, y, title, sub, target=None, size=40, width=None, anchor_side=None):
    """Box at (x, y) with a title and sub line; a dashed gold line and arrow to target."""
    over = Image.new('RGBA', im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    ft, fs = font(True, size), font(False, int(size * 0.72))
    w = width or int(max(d.textlength(title, font=ft), d.textlength(sub, font=fs) if sub else 0) + size * 1.2)
    h = int(size * (2.45 if sub else 1.55))
    d.rounded_rectangle((x, y, x + w, y + h), radius=int(size * 0.35), fill=BOX, outline=GOLD + (235,), width=3)
    d.text((x + size * 0.55, y + size * 0.28), title, font=ft, fill=HOT + (255,))
    if sub:
        d.text((x + size * 0.57, y + size * 1.42), sub, font=fs, fill=TXT + (240,))
    if target:
        side = anchor_side or ('right' if target[0] > x + w else 'left' if target[0] < x else 'bottom')
        sx, sy = {'right': (x + w, y + h / 2), 'left': (x, y + h / 2), 'bottom': (x + w / 2, y + h),
                  'top': (x + w / 2, y)}[side]
        tx, ty = target
        dist = math.hypot(tx - sx, ty - sy)
        ux, uy = (tx - sx) / dist, (ty - sy) / dist
        t, end = 0.0, dist - 22
        while t < end:
            d.line((sx + ux * t, sy + uy * t, sx + ux * min(t + 16, end), sy + uy * min(t + 16, end)),
                   fill=GOLD + (240,), width=5)
            t += 26
        px, py = -uy, ux
        bx, by = tx - ux * 26, ty - uy * 26
        d.polygon([(tx, ty), (bx + px * 12, by + py * 12), (bx - px * 12, by - py * 12)], fill=GOLD + (250,))
    im.alpha_composite(over)


def wordmark(im, x=28, y=None, sub='Town Portals for Enshrouded'):
    over = Image.new('RGBA', im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    wm, ws = font(True, 44), font(False, 24)
    w = int(max(d.textlength('TOWN PORTALS', font=wm), d.textlength(sub, font=ws)) + 52)
    y = im.height - 128 if y is None else y
    d.rounded_rectangle((x, y, x + w, y + 100), radius=14, fill=(22, 18, 14, 200), outline=GOLD + (210,), width=3)
    d.text((x + 26, y + 8), 'TOWN PORTALS', font=wm, fill=HOT + (255,))
    d.text((x + 28, y + 62), sub, font=ws, fill=TXT + (235,))
    im.alpha_composite(over)


def save(im, name, w=None):
    im = im.convert('RGB')
    if w and im.width > w:
        im = im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)
    im.save(os.path.join(OUT, name), quality=90)
    print(name, im.size)


os.makedirs(OUT, exist_ok=True)

# Header 1300x372: the banner's top; Nexus covers its lower ~45% with the title and stats, so
# the wordmark (painted in the banner) stays in the top part.
banner = Image.open(os.path.join(SRC, 'banner-dungeon-escape.png')).convert('RGB')
crop_h = round(372 * banner.width / 1300)
banner.crop((0, 0, banner.width, crop_h)).resize((1300, 372), Image.LANCZOS).save(os.path.join(OUT, 'header-1300x372.jpg'), quality=92)
print('header-1300x372.jpg (1300, 372)')

# 01 the banner, as it is.
save(Image.open(os.path.join(SRC, 'banner-dungeon-escape.png')).convert('RGBA'), '01-banner.jpg')

# 02 a portal home from a mountain peak: your altar's fire through it, the travel prompt (1.0.3).
im = sky_fill(load('chat135-peak-portal.png'), (1255, 334, 1460, 381)).crop((0, 132, 2311, 1300))
callout(im, 1560, 300, 'Your Flame Altar, right through it',
        'the portal shows where it leads, with depth as you move', target=(1335, 420), size=40)
callout(im, 1560, 820, 'Press E to travel home', 'it names your altar and where it stands',
        target=(1300, 572), size=40)
wordmark(im)
save(im, '02-portal-home.jpg', 2000)

# 03 a long trip, half loaded: from a portal in Cinder's Cradle home to the Springlands altar.
# The shot is small (the map is ~450 px high), so it is cropped with its backdrop and doubled.
im = load('chat123.png').crop((500, 30, 1500, 556))
im = im.resize((im.width * 2, im.height * 2), Image.LANCZOS)
callout(im, 1490, 40, 'Where you opened it', "your portal, in Cinder's Cradle", target=(1092, 140), size=34)
callout(im, 1490, 400, 'The path grows', 'as the destination loads', target=(1010, 480), size=34)
callout(im, 20, 680, 'Your Flame Altar', "where you'll arrive", target=(912, 775), size=34)
callout(im, 1490, 900, 'Loading progress', 'shown as you travel', target=(1092, 1010), size=34)
save(im, '03-travel-map.jpg', 2000)

# 03b the same part of the map in English (the painting as made) and in Thai (assets/map/map-th.jpg at
# full size: tools/map/letter_map.py writes it to tools/map/out/map-th-full.png).
MAP = os.path.join(HERE, '..', 'assets', 'map')
box = (213, 414, 815, 1166)
english = Image.open(os.path.join(MAP, 'map-source-en.png')).convert('RGBA').crop(box)
thai = Image.open(os.path.join(HERE, 'map', 'out', 'map-th-full.png')).convert('RGBA').crop(box)
h = 1000
english = english.resize((round(english.width * h / english.height), h), Image.LANCZOS)
thai = thai.resize((round(thai.width * h / thai.height), h), Image.LANCZOS)
top, gap = 120, 16
im = Image.new('RGBA', (english.width + thai.width + gap, h + top), (30, 30, 52, 255))
im.alpha_composite(english, (0, top))
im.alpha_composite(thai, (english.width + gap, top))
d = ImageDraw.Draw(im)
d.rectangle((english.width, top, english.width + gap - 1, top + h), fill=GOLD)
thai_font = ImageFont.truetype('C:/Windows/Fonts/LeelawUI.ttf', 44)
d.text((30, 22), 'English', font=font(True, 52), fill=HOT)
d.text((32, 80), 'the map as painted', font=font(False, 28), fill=TXT)
x = english.width + gap + 30
d.text((x, 22), 'Thai', font=font(True, 52), fill=HOT)
d.text((x + d.textlength('Thai', font=font(True, 52)) + 24, 24), 'ภาษาไทย', font=thai_font, fill=HOT)
d.text((x + 2, 80), "lettered in the game's language, in all 15", font=font(False, 28), fill=TXT)
save(im, '03b-map-languages.jpg', 2000)

# 04 before the first trip: a landmark of the region in the altar's portal (1.0.1 capture at the
# altar; the quest markers of the villagers behind the wall are painted out).
markers = [(228, 331), (468, 313), (868, 385), (675, 497), (1084, 529)]
im = load('chat129-default-picture.png', erase=[(x - 40, y - 46, x + 40, y + 46) for x, y in markers] + [(743, 151, 911, 188)]).crop((0, 115, 2185, 1300))
callout(im, 1240, 50, 'Before your first trip there', 'a landmark of that region, by day or by night',
        target=(860, 380), size=44)
callout(im, 70, 820, 'Closes behind you', 'when you return through it', target=(655, 640), size=40)
save(im, '04-default-picture.jpg', 2000)

# 05 the altar's twin: beside the Flame Altar at home, showing the peak the portal was opened on.
im = load('chat136-altar-portal.png', erase=[(958, 155, 1128, 192)]).crop((560, 180, 2690, 1300))
callout(im, 1180, 70, 'And back again', 'the twin by your altar shows the peak you left',
        target=(330, 300), size=44)
callout(im, 1180, 860, 'Walk through to return', 'right where you opened it, mid-adventure', target=(560, 640), size=40)
wordmark(im)
save(im, '05-anywhere.jpg', 2000)

# 06 the second banner.
save(Image.open(os.path.join(SRC, 'banner-panic-run.png')).convert('RGBA'), '06-banner-panic-run.jpg')

# 07 the escape: a skeleton horde in a crypt chasing the player to the portal home (1.0.3, FSR3).
im = load('chat132-dungeon-escape.png', erase=[(1662, 543, 1848, 580)]).crop((725, 130, 2787, 1290))
callout(im, 60, 110, 'When the dungeon turns on you', 'your way home is already open', target=(930, 560), size=44)
callout(im, 60, 880, "Press E and you're gone", 'the horde stays behind', target=(950, 735), size=40)
save(im, '07-dungeon-escape.jpg', 2000)
