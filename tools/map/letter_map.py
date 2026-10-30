"""A translated loading-screen map from a relettered copy of the painting.

The relettered copy (out/relettered-<code>.png: map-source-en.png with its scroll text redrawn in one
language, label-names.json) is lined up with the painting (ORB features, then ECC), and only the
scrolls (erase_labels.LABELS boxes, a little wider, feathered) are taken from it; the rest of the map
stays the original painting, so the map-to-world warp still fits. Writes assets/map/map-<code>.jpg
(1024 px, as make_map.py) and out/letter-check-<code>.jpg: each scroll, English beside the new one.
label-names.json is the game's own text (its loca tables, 2026-10-30).
Run: python letter_map.py <code> [relettered image] [relettered image for the spire scrolls]
"""
import os
import sys
import cv2
import numpy as np
from PIL import Image

from erase_labels import LABELS

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = os.path.join(HERE, '..', '..', 'assets', 'map')
OUT = os.path.join(HERE, 'out')
SOURCE_EN = os.path.join(MAP, 'map-source-en.png')
SIZE = 1024
MARGIN = 14  # beyond each label box: a scroll stretched for a longer name
FEATHER = 6
REACH = 70  # how far beside its box a stretched scroll may reach
TALLER = 16  # how far above and below (two-line spire scrolls)
# Letters the relettering kept getting wrong, fixed with the same letter from another scroll of
# the same size, by image (its pixels): (source box, destination top-left). Thai: ChatGPT drew ศ
# for the ค in สรรค์ (Pillars of Creation) twice; the ค of คินเดิลเวสต์ (Kindlewastes Spire) replaces it.
# Japanese: the 古 and 塔 of 古の尖塔 on the Veilwater spire came out garbled; the Blackmire spire's replace them.
# A destination box instead of a top-left scales the source to it. Korean: the Veilwater spire's second
# line stayed ambiguous at its size (베일 or 베임, 역 or 억) after two corrections; the region scroll's
# 베일워터 유역 above it, in the same lettering, scaled down replaces it.
PATCHES = {
    'relettered-th.png': [((1057, 947, 1067, 958), (664, 762))],
    'spires-ja.png': [((520, 598, 537, 616), (594, 448)), ((479, 597, 495, 616), (549, 448))],
    'relettered-ko2.png': [((353, 351, 499, 373), (543, 472, 616, 486))],
    # ('text', what, font, box): the box's lettering written anew in a system font. Simplified Chinese:
    # ChatGPT drew 禍豬 for the 褐赭 of 褐赭空洞 (Umber Hollow) twice; no other scroll has those characters.
    'relettered-zh-hans2.png': [('text', '褐赭空洞', 'C:/Windows/Fonts/msyhbd.ttc', (762, 897, 845, 921))],
}


def write(out, what, font, box):
    """what in font, as large as fits box, in the ink of the lettering it replaces."""
    from PIL import ImageDraw, ImageFont
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    old = out[y0:y1, x0:x1]
    dark = cv2.cvtColor(old, cv2.COLOR_RGB2GRAY) < 110
    ink = np.median(old[dark], axis=0) if dark.any() else np.array([45, 30, 20])
    region = out[y0 - 6:y1 + 6, x0 - 6:x1 + 6].copy()
    mask = np.zeros(region.shape[:2], np.uint8)
    mask[6:6 + h, 6:6 + w] = cv2.dilate((cv2.cvtColor(old, cv2.COLOR_RGB2GRAY) < 150).astype(np.uint8), np.ones((3, 3), np.uint8))
    out[y0 - 6:y1 + 6, x0 - 6:x1 + 6] = cv2.inpaint(region, mask, 3, cv2.INPAINT_TELEA)
    big = 4  # drawn at 4x, then reduced: smooth edges
    face = ImageFont.truetype(font, h * big)
    l, t, r, b = face.getbbox(what)
    scale = min(w * big / (r - l), h * big / (b - t))
    face = ImageFont.truetype(font, max(8, int(h * big * scale)))
    l, t, r, b = face.getbbox(what)
    canvas = Image.new('L', (w * big, h * big), 0)
    ImageDraw.Draw(canvas).text(((w * big - (r - l)) / 2 - l, (h * big - (b - t)) / 2 - t), what, font=face, fill=255)
    alpha = np.asarray(canvas.resize((w, h), Image.LANCZOS), np.float32)[..., None] / 255 * 0.95
    out[y0:y1, x0:x1] = (out[y0:y1, x0:x1] * (1 - alpha) + ink * alpha + 0.5).astype(np.uint8)


def patch(gen, name):
    """Each patch: the destination's ink painted out with its parchment, then the source's ink laid on."""
    out = gen.copy()
    for entry in PATCHES.get(name, []):
        if entry[0] == 'text':
            write(out, *entry[1:])
            continue
        (x0, y0, x1, y1), to = entry
        dx, dy = to[:2]
        w, h = (to[2] - dx, to[3] - dy) if len(to) == 4 else (x1 - x0, y1 - y0)
        dest = out[dy:dy + h, dx:dx + w]
        ink = cv2.dilate((cv2.cvtColor(dest, cv2.COLOR_RGB2GRAY) < 150).astype(np.uint8), np.ones((3, 3), np.uint8))
        region = out[dy - 6:dy + h + 6, dx - 6:dx + w + 6].copy()
        mask = np.zeros(region.shape[:2], np.uint8)
        mask[6:6 + h, 6:6 + w] = ink
        out[dy - 6:dy + h + 6, dx - 6:dx + w + 6] = cv2.inpaint(region, mask, 3, cv2.INPAINT_TELEA)
        src = cv2.resize(gen[y0:y1, x0:x1], (w, h), interpolation=cv2.INTER_AREA).astype(np.float32)
        paper = np.median(gen[y0 - 2:y1 + 2, x0 - 2:x1 + 2].reshape(-1, 3), axis=0)
        alpha = np.clip((paper.mean() - src.mean(axis=2)) / (paper.mean() - 25), 0, 1)
        if len(to) == 4:
            # A scaled line: ink along the source box's edge and across much of it is the scroll's outline.
            count, parts, stats, _ = cv2.connectedComponentsWithStats((alpha > 0.15).astype(np.uint8))
            for i in range(1, count):
                x, y, cw, ch = stats[i, :4]
                if cw >= 0.4 * w and (y == 0 or y + ch == h):
                    alpha[parts == i] = 0
        alpha = alpha[..., None]
        dest = out[dy:dy + h, dx:dx + w].astype(np.float32)
        out[dy:dy + h, dx:dx + w] = (dest * (1 - alpha) + src * alpha + 0.5).astype(np.uint8)
    return out


def align(gen, src):
    """gen warped onto src's pixels: a homography from ORB matches, refined by ECC."""
    h, w = src.shape[:2]
    g0 = cv2.cvtColor(gen, cv2.COLOR_RGB2GRAY)
    s0 = cv2.cvtColor(src, cv2.COLOR_RGB2GRAY)
    orb = cv2.ORB_create(8000)
    kg, dg = orb.detectAndCompute(g0, None)
    ks, ds = orb.detectAndCompute(s0, None)
    matches = sorted(cv2.BFMatcher(cv2.NORM_HAMMING, crossCheck=True).match(dg, ds), key=lambda m: m.distance)[:1500]
    pg = np.float32([kg[m.queryIdx].pt for m in matches])
    ps = np.float32([ks[m.trainIdx].pt for m in matches])
    H, inliers = cv2.findHomography(pg, ps, cv2.RANSAC, 3.0)
    warped = cv2.warpPerspective(gen, H, (w, h), flags=cv2.INTER_CUBIC)
    # ECC on the result, text-free areas weigh as much as the rest; small residual only.
    warp = np.eye(3, dtype=np.float32)
    g1 = cv2.cvtColor(warped, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255
    try:
        _, warp = cv2.findTransformECC(s0.astype(np.float32) / 255, g1, warp, cv2.MOTION_HOMOGRAPHY,
                                       (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 200, 1e-6), None, 5)
        warped = cv2.warpPerspective(warped, warp, (w, h), flags=cv2.INTER_CUBIC | cv2.WARP_INVERSE_MAP)
    except cv2.error:
        pass
    return warped, int(inliers.sum())


def main():
    code = sys.argv[1]
    gen_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(OUT, f'relettered-{code}.png')
    # A later relettering for the spire scrolls only (the region scrolls kept from the first).
    spire_path = sys.argv[3] if len(sys.argv) > 3 else None
    src = np.array(Image.open(SOURCE_EN).convert('RGB'))
    h, w = src.shape[:2]
    sources = []
    for path in (gen_path, spire_path):
        if not path:
            continue
        warped, inliers = align(patch(np.array(Image.open(path).convert('RGB')), os.path.basename(path)), src)
        # What the relettering changed: a scroll stretched for a longer name grows its box.
        changed = cv2.GaussianBlur(np.abs(warped.astype(np.float32) - src.astype(np.float32)).mean(axis=2), (0, 0), 2) > 28
        sources.append((warped, inliers, cv2.morphologyEx(changed.astype(np.uint8), cv2.MORPH_CLOSE, np.ones((5, 5), np.uint8))))
    masks = [np.zeros((h, w), np.float32) for _ in sources]
    boxes = {}
    for name, (x0, y0, x1, y1) in LABELS.items():
        k = 1 if len(sources) > 1 and name.endswith('Spire') else 0
        changed = sources[k][2]
        # Each scroll's area: its box, grown to what changed round it within REACH (TALLER up and down).
        bx0, by0, bx1, by1 = max(0, x0 - REACH), max(0, y0 - TALLER), min(w, x1 + REACH), min(h, y1 + TALLER)
        near = changed[by0:by1, bx0:bx1]
        count, parts = cv2.connectedComponents(near)
        core = np.zeros_like(near)
        core[y0 - by0:y1 - by0, x0 - bx0:x1 - bx0] = 1
        grown = np.zeros_like(near)
        for i in range(1, count):
            if (core & (parts == i)).any():
                grown |= (parts == i).astype(np.uint8)
        ys, xs = np.nonzero(grown)
        gx0, gx1 = (min(x0, bx0 + xs.min()), max(x1, bx0 + xs.max() + 1)) if len(xs) else (x0, x1)
        gy0, gy1 = (min(y0, by0 + ys.min()), max(y1, by0 + ys.max() + 1)) if len(ys) else (y0, y1)
        boxes[name] = (max(0, gx0 - MARGIN), max(0, gy0 - MARGIN), min(w, gx1 + MARGIN), min(h, gy1 + MARGIN))
        bx0, by0, bx1, by1 = boxes[name]
        masks[k][by0:by1, bx0:bx1] = 1
    for k in range(1, len(masks)):
        masks[0][masks[k] > 0] = 0  # a spire box over a region box: the spire image there
    softs = [cv2.GaussianBlur(m, (0, 0), FEATHER) for m in masks]
    total = np.minimum(sum(softs), 1)
    out = src * (1 - total[..., None])
    for (warped, _, _), soft in zip(sources, softs):
        out = out + warped * (soft / np.maximum(sum(softs), 1e-6) * total)[..., None]
    out = (out + 0.5).clip(0, 255).astype(np.uint8)
    # How well it lines up away from the scrolls: mean colour difference there.
    outside = sum(masks) == 0
    diff = np.abs(sources[0][0].astype(int) - src.astype(int)).mean(axis=2)[outside].mean()
    inliers = ' + '.join(str(i) for _, i, _ in sources)
    texture = os.path.join(MAP, f'map-{code}.jpg')
    Image.fromarray(out).resize((SIZE, SIZE), Image.LANCZOS).save(texture, quality=90, optimize=True, subsampling=0)
    # Check sheet: each scroll, English | new, at 2x.
    rows = []
    for name, (x0, y0, x1, y1) in boxes.items():
        a, b = src[y0:y1, x0:x1], out[y0:y1, x0:x1]
        row = np.hstack([a, np.full((a.shape[0], 6, 3), 255, np.uint8), b])
        rows.append(cv2.resize(row, None, fx=2, fy=2, interpolation=cv2.INTER_CUBIC))
    width = max(r.shape[1] for r in rows)
    sheet = np.vstack([np.pad(r, ((0, 6), (0, width - r.shape[1]), (0, 0)), constant_values=40) for r in rows])
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray(sheet).save(os.path.join(OUT, f'letter-check-{code}.jpg'), quality=88)
    print(f'map-{code}.jpg {os.path.getsize(texture) // 1024} KB; {inliers} feature matches; '
          f'mean difference off the scrolls {diff:.1f} (0-255)')


if __name__ == '__main__':
    main()
