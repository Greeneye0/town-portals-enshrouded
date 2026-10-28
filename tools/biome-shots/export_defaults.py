"""Export the biome landmark shots as the mod's default altar-portal pictures.

out/<day|night>/<region>_<landmark>.png (2300x1150, crop.py) -> assets/biomes/<region>-<day|night>.jpg
at 2048x1024, the portal snapshot size. Region names are the first word of each shot's name,
matching portal_default_images.cpp. Night shots are brightened to a common level (mean linear
light NIGHT_MEAN, gain at most MAX_GAIN), so the landmark reads in a small portal while it still
looks like night; real snapshots are not touched. Each picture fills the whole snapshot view
(portal_default_view.h): the photo in the middle at 1/PHOTO_SCALE of the frame, the area
round it the photo itself enlarged and blurred, so a portal seen from any angle never shows
past the picture. Run: python export_defaults.py
"""
import os
import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.join(HERE, '..', '..', 'assets', 'biomes')
NIGHT_MEAN, MAX_GAIN = 0.05, 8.0
W, H, PHOTO_SCALE, FEATHER = 2048, 1024, 1.8, 96
REGIONS = ('springlands', 'revelwood', 'blackmire', 'nomad', 'kindlewastes', 'albaneve', 'veilwater', 'cinders')


def brighten(im):
    a = np.asarray(im, dtype=np.float32) / 255
    lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    luma = (lin * [0.2126, 0.7152, 0.0722]).sum(-1).mean()
    gain = min(MAX_GAIN, max(1.0, NIGHT_MEAN / max(luma, 1e-4)))
    lin = lin * gain
    lin = lin / (1 + np.maximum(lin - 0.8, 0) * 2)  # soft roll-off instead of clipping highlights
    out = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(np.clip(lin, 0, 1), 1 / 2.4) - 0.055)
    return Image.fromarray(np.clip(out * 255 + 0.5, 0, 255).astype(np.uint8)), gain


def frame(photo):
    """The photo centred in the full view; round it, the photo itself enlarged to fill the view
    and heavily blurred, so the colours carry on without hard edges."""
    pw, ph = photo.size
    x0, y0 = (W - pw) // 2, (H - ph) // 2
    small = photo.resize((W // 8, H // 8), Image.BILINEAR)
    for _ in range(3):  # blur at a small size (fast), several passes
        small = small.filter(ImageFilter.GaussianBlur(5))
    back = small.resize((W, H), Image.BICUBIC)
    mask = Image.new('L', (W, H), 0)
    mask.paste(Image.new('L', (pw - 2 * FEATHER, ph - 2 * FEATHER), 255), (x0 + FEATHER, y0 + FEATHER))
    mask = mask.filter(ImageFilter.GaussianBlur(FEATHER / 2))
    full = Image.new('RGB', (W, H))
    full.paste(photo, (x0, y0))
    return Image.composite(full, back, mask)


def main():
    os.makedirs(DEST, exist_ok=True)
    total = 0
    for phase in ('day', 'night'):
        src = os.path.join(HERE, 'out', phase)
        shots = {f.split('_')[0]: f for f in os.listdir(src) if f.endswith('.png')}
        for region in REGIONS:
            assert region in shots, f'no {phase} shot for {region}'
            size = (round(W / PHOTO_SCALE), round(H / PHOTO_SCALE))
            im = Image.open(os.path.join(src, shots[region])).convert('RGB').resize(size, Image.LANCZOS)
            gain = 1.0
            if phase == 'night':
                im, gain = brighten(im)
            im = frame(im)
            path = os.path.join(DEST, f'{region}-{phase}.jpg')
            im.save(path, quality=88, optimize=True, subsampling=0)
            total += os.path.getsize(path)
            print(f'{region}-{phase}.jpg  <- {shots[region]}  {os.path.getsize(path) // 1024} KB  gain {gain:.1f}')
    print(f'total {total / 1048576:.1f} MB')


if __name__ == '__main__':
    main()
