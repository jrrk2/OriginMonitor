#!/usr/bin/env python3
"""Stack the 30 one-millisecond Jupiter frames (ISO 100) of 2026-10-12 06:59-07:02 BST: debayer (GRBG) to half resolution, centre on the disc, average."""
import glob, numpy as np
from astropy.io import fits
from scipy import ndimage as ndi
from PIL import Image
D = '/mnt/fits-backup/claude/dawn_jupiter_20261012/seq30/'; BLACK = 4985.0
import cv2
def load(f):                                                                         # raw GRBG mosaic -> full-resolution RGB (OpenCV's name for this layout is BayerGB)
    d = np.clip(fits.open(f)[0].data.astype(np.float32) - BLACK, 0, 65535).astype(np.uint16)
    return cv2.cvtColor(d, cv2.COLOR_BayerGB2RGB_EA).astype(np.float64)
fs = sorted(glob.glob(D + '*.fits')); acc = None; n = 0; first = None
for f in fs:
    im = load(f); L = im.sum(2); L = ndi.gaussian_filter(L, 4)
    m = L > 0.5 * L.max(); cy, cx = ndi.center_of_mass(L * m)
    s = ndi.shift(im, (im.shape[0] / 2 - cy, im.shape[1] / 2 - cx, 0), order=1)
    acc = s if acc is None else acc + s; n += 1
    if first is None: first = s
def crop(a, r=48): h, w = a.shape[:2]; return a[h // 2 - r:h // 2 + r, w // 2 - r:w // 2 + r]
stack = acc / n
bg = np.median(stack.reshape(-1, 3), axis=0); stack = stack - bg; first = first - bg      # background subtraction
refl = crop(stack, 14).sum((0, 1)); gain = refl.max() / refl; stack = stack * gain; first = first * gain      # equal disc channels: colour is only roughly balanced
def stretch(a, hi): return np.clip(a / hi, 0, 1) ** 1.0
hi = crop(stack).max() * 1.1
np.save('/tmp/claude-1000/-home-jonathan-OriginMonitor/cdc64fb7-0772-48f7-8ac1-4911d0c24f92/scratchpad/jup_stack.npy', stack)
for name, a in (('jupiter_single', first), ('jupiter_stack30', stack)):
    c = stretch(crop(a), hi); Image.fromarray((c * 255).astype('uint8')).resize((576, 576), Image.LANCZOS).save('figs/%s.png' % name)
print(n, 'frames', stack.shape, 'peak', crop(stack).max())
