#!/usr/bin/env python3
"""The conventional stack of the same registered frames, for comparison with the drizzle: bilinear debayer of every frame, bilinear alignment at the native pixel scale (1.478 arcsec/px), the same
weights and background subtraction, plain weighted mean. Picture: the same stretch reference as the drizzle picture (taken from the drizzle) and the same display size (bicubic enlargement).

    plain_stack.py --window X0 Y0 W H --list frames.txt --reg reg.pkl --drz drz_all.npy --out name.png [--crop Y0 Y1 X0 X1 (drizzle px)] [--zoom N]
    writes name.png (8-bit preview), name.tif (16-bit stretched, same display size as the drizzle pictures) and name_linear.tif (float32, linear, native scale)
"""
import argparse, json, os, pickle, sys
import numpy as np, cv2
from scipy.ndimage import convolve
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m57

ap = argparse.ArgumentParser(); ap.add_argument('--window', type=int, nargs=4, required=True); ap.add_argument('--list', required=True); ap.add_argument('--reg', required=True); ap.add_argument('--drz', required=True)
ap.add_argument('--out', required=True); ap.add_argument('--crop', type=int, nargs=4); ap.add_argument('--zoom', type=int, default=1); a = ap.parse_args()
m57.set_window(*a.window); WX, WY = m57.WX, m57.WY
files = open(a.list).read().split(); Ts, ok, _ = pickle.load(open(a.reg, 'rb')); hfr = json.load(open(a.list + '.hfr.json'))
K = np.array([[1, 2, 1], [2, 4, 2], [1, 2, 1]], float); masks = [(m57.CH == k).astype(float) for k in range(3)]; norm = [convolve(m, K, mode='nearest') for m in masks]     # bilinear (normalised) debayer
SUM = np.zeros((3, WY, WX)); WS = np.zeros((3, WY, WX)); n = 0
for i, f in enumerate(files):
    if not ok[i]: continue
    c = m57.load(f).astype(np.float64); planes = []; sig = []
    for k in range(3):
        v = c[m57.CH == k]; bg = np.median(v); sig.append(1.4826 * np.median(abs(v - bg))); planes.append(convolve((c - bg) * masks[k], K, mode='nearest') / norm[k])
    w = (2.3 / hfr[f]) ** 2 / float(np.mean(sig)) ** 2
    cover = cv2.warpAffine(np.ones((WY, WX), np.float32), Ts[i], (WX, WY), flags=cv2.INTER_LINEAR)
    for k in range(3):
        SUM[k] += w * cv2.warpAffine(planes[k].astype(np.float32), Ts[i], (WX, WY), flags=cv2.INTER_LINEAR) * (cover > 0.999); WS[k] += w * (cover > 0.999)
    n += 1
mean = SUM / np.maximum(WS, 1e-12); print(n, 'frames stacked at native scale', mean.shape)
rgb_d, L_d, _ = m57.rgbl(np.load(a.drz)); y0, y1, x0, x1 = a.crop or (0, L_d.shape[0], 0, L_d.shape[1]); top = np.percentile(L_d[y0:y1, x0:x1], 99.7)   # the drizzle's stretch reference
rgb = np.stack([mean[1], mean[0], mean[2]]); rgb = rgb - np.array([np.median(rgb[k]) for k in range(3)])[:, None, None]
st = lambda x: np.clip(np.arcsinh(10 * np.clip(x, 0, None) / top) / np.arcsinh(10), 0, 1)
img = np.stack([st(rgb[k][y0 // 2:y1 // 2, x0 // 2:x1 // 2]) for k in range(3)], -1)
fac = 2 * a.zoom; img = cv2.resize(img, None, fx=fac, fy=fac, interpolation=cv2.INTER_CUBIC).clip(0, 1)
import tifffile
base = a.out[:-4]
cv2.imwrite(a.out, (img[:, :, ::-1] * 255).astype(np.uint8))                                                    # 8-bit preview
tifffile.imwrite(base + '.tif', (img * 65535).round().astype(np.uint16))                                          # 16-bit, stretched, same display size (display product)
tifffile.imwrite(base + '_linear.tif', np.moveaxis(rgb, 0, -1).astype(np.float32))                                # 32-bit float, linear, native pixel scale, background-subtracted R G B
print('wrote', a.out, base + '.tif', base + '_linear.tif', img.shape)
