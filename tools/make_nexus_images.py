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

# 02 a portal home, from the sky ruins: your altar seen through it, the travel prompt.
im = load('chat119.png', erase=[(770, 108, 985, 172)]).crop((40, 100, 2000, 1090))
callout(im, 1060, 60, 'Your Flame Altar, right through it',
        'the portal shows where it leads, with depth as you move', target=(890, 290), size=40)
callout(im, 1060, 560, 'Press E to travel home', 'it names your altar and where it stands',
        target=(975, 565), size=40)
wordmark(im)
save(im, '02-portal-home.jpg', 2000)

# 03 the loading screen: the trip on a painted world map, in the game's language.
im = load('chat117.png').crop((250, 60, 1900, 1330))
callout(im, 20, 1000, 'The trip, on a painted world map', 'the path grows as the destination loads',
        target=(1145, 948), size=42, anchor_side='right')
callout(im, 1120, 640, 'Your portal', 'mist and motes, in your colour', target=(1275, 905), size=36, anchor_side='bottom')
callout(im, 20, 30, 'In the game\'s language', 'labels from the game itself (Thai here)', target=(580, 290), size=40)
callout(im, 20, 760, 'Your Flame Altar', 'its flame flickers', target=(690, 890), size=40)
save(im, '03-travel-map.jpg', 2000)

# 03b the same part of the map in English (the painting as made) and in Thai (drawn by the mod).
english = load('chat120.png').crop((414, 494, 995, 1174))
thai = load('chat117.png').crop((544, 251, 1248, 1074))
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
d.text((x + 2, 80), "labels written in the game's language, in all 15", font=font(False, 28), fill=TXT)
save(im, '03b-map-languages.jpg', 2000)

# 04 before the first trip: a landmark of the region in the altar's portal.
im = load('chat98.png').crop((300, 70, 1800, 860))
callout(im, 820, 40, 'Before your first trip there', 'a landmark of that region, by day or by night',
        target=(400, 360), size=44)
callout(im, 820, 560, 'Closes behind you', 'when you return through it', target=(150, 650), size=40)
save(im, '04-default-picture.jpg', 2000)

# 05 home from anywhere: a portal over lava, the prompt in the game's language.
im = load('chat114.png', erase=[(520, 110, 710, 152)]).crop((0, 90, 1450, 1300))
callout(im, 820, 120, 'Home from anywhere', 'one portal per player, opened with T', target=(640, 420), size=44)
callout(im, 820, 880, 'Every text in 15 languages', 'Thai here', target=(700, 590), size=40)
wordmark(im)
save(im, '05-anywhere.jpg', 1600)

# 06 the second banner.
save(Image.open(os.path.join(SRC, 'banner-panic-run.png')).convert('RGBA'), '06-banner-panic-run.jpg')
