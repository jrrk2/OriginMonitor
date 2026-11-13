#!/usr/bin/env python3
"""A crossfade animation between two stacks on the same grid (e.g. broadband and narrowband of one target), to show what each one records.

    tools/crossfade.py A_RGB.fits B_RGB.fits OUT_BASENAME [--labels "Broadband|Quad-band filter"] [--centre X,Y] [--size 900x600] [--scale 1.0]
                       [--hold 1.5] [--fade 3.0] [--fps 20]

A and B are RGB.fits from tools/fullres_stack.py on one reference grid (planes R, G, B). Each is stretched on its own (sky to the same grey, the
target's bright core to the same level, asinh), so the fade shows structure and colour, not exposure. Sequence: hold A, fade A -> B (smoothstep),
hold B, fade back. Writes OUT_BASENAME.gif (animated, looping) and OUT_BASENAME.webp (smaller), plus the two end frames as .jpg.
"""
import argparse
import numpy as np


def stretch(rgb, box):
    import cv2
    x0, y0, x1, y1 = box
    c = np.nan_to_num(rgb[y0:y1, x0:x1].astype(np.float32))
    for k in range(3): c[..., k] -= np.median(c[..., k])                         # neutral sky
    lum = c.mean(axis=2)
    sky_sd = 1.4826 * np.median(np.abs(lum - np.median(lum)))
    hi = np.percentile(cv2.GaussianBlur(lum, (0, 0), 3), 99.7)                  # the target's bright part (smoothed, so not a star)
    lo = -2.0 * sky_sd
    soft = max((hi - lo) / 40.0, 1e-6)
    out = np.arcsinh(np.clip(c - lo, 0, None) / soft) / np.arcsinh((hi - lo) / soft)
    return np.clip(out, 0, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a'); ap.add_argument('b'); ap.add_argument('out')
    ap.add_argument('--labels', default='Broadband (no filter)|Antlia quad-band nebula filter')
    ap.add_argument('--centre'); ap.add_argument('--size', default='900x600'); ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--hold', type=float, default=1.5); ap.add_argument('--fade', type=float, default=3.0); ap.add_argument('--fps', type=int, default=12)
    a = ap.parse_args()
    import cv2
    from astropy.io import fits
    from PIL import Image, ImageDraw, ImageFont
    A = np.moveaxis(fits.getdata(a.a).astype(np.float32), 0, 2); B = np.moveaxis(fits.getdata(a.b).astype(np.float32), 0, 2)
    H, W = A.shape[:2]; w, h = (int(v) for v in a.size.split('x'))
    cx, cy = (int(v) for v in a.centre.split(',')) if a.centre else (W // 2, H // 2)
    box = (max(0, cx - w // 2), max(0, cy - h // 2), min(W, cx + w // 2), min(H, cy + h // 2))
    sa, sb = stretch(A, box), stretch(B, box)
    if a.scale != 1.0:
        size = (int(sa.shape[1] * a.scale), int(sa.shape[0] * a.scale))
        sa = cv2.resize(sa, size, interpolation=cv2.INTER_AREA); sb = cv2.resize(sb, size, interpolation=cv2.INTER_AREA)
    la, lb = a.labels.split('|')
    try: font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', max(14, sa.shape[0] // 28))
    except OSError: font = ImageFont.load_default()

    def frame(t):
        """t = 0 (A) .. 1 (B), smoothstep in between; both labels fade with it"""
        s = t * t * (3 - 2 * t)
        im = Image.fromarray((np.clip((1 - s) * sa + s * sb, 0, 1) * 255).astype(np.uint8)).convert('RGBA')
        y = im.height - font.size - 14
        for text, alpha in ((la, 1 - s), (lb, s)):                       # each label on its own layer, so its transparency really blends
            if alpha <= 0.01: continue
            layer = Image.new('RGBA', im.size, (0, 0, 0, 0))
            ImageDraw.Draw(layer).text((14, y), text, font=font, fill=(255, 255, 255, int(255 * alpha)))
            im = Image.alpha_composite(im, layer)
        return im.convert('RGB')

    nh, nf = int(a.hold * a.fps), int(a.fade * a.fps)
    ts = [0.0] * nh + [i / (nf - 1) for i in range(nf)] + [1.0] * nh + [1 - i / (nf - 1) for i in range(nf)]
    frames = [frame(t) for t in ts]
    dur = int(1000 / a.fps)
    frames[0].save(a.out + '.webp', save_all=True, append_images=frames[1:], duration=dur, loop=0, quality=88, method=4)
    frames[0].save(a.out + '.gif', save_all=True, append_images=[f.convert('P', palette=Image.ADAPTIVE, colors=255) for f in frames[1:]], duration=dur, loop=0, optimize=True)
    frame(0.0).save(a.out + '_a.jpg', quality=92); frame(1.0).save(a.out + '_b.jpg', quality=92)
    print(f"{len(frames)} frames, {len(frames) * dur / 1000:.1f} s loop: {a.out}.webp, {a.out}.gif")


if __name__ == '__main__':
    main()
