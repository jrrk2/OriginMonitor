#!/usr/bin/env python3
"""usage: starshape.py frame.fits [...] - star ellipticity and orientation vs position (half-res green plane)"""
import sys, numpy as np, cv2
from astropy.io import fits
from scipy import ndimage as ndi
for fn in sys.argv[1:]:
    d = fits.getdata(fn).astype(np.float32); g = (d[0::2, 0::2] + d[1::2, 1::2]) / 2 - 4985
    h = cv2.GaussianBlur(g, (0, 0), 1.2) - cv2.GaussianBlur(g, (0, 0), 10); s = np.std(h)
    pk = (h == ndi.maximum_filter(h, 17)) & (h > 15 * s) & (g < 55000)
    ys, xs = np.nonzero(pk); H, W = g.shape; cx, cy = W / 2, H / 2; R = 7
    rows = []
    for y, x in zip(ys, xs):
        if x < R + 2 or y < R + 2 or x >= W - R - 2 or y >= H - R - 2: continue
        c = g[y - R:y + R + 1, x - R:x + R + 1]; bg = np.median(np.r_[c[0], c[-1], c[:, 0], c[:, -1]]); c = np.clip(c - bg, 0, None)
        if c.sum() <= 0 or c.max() > 60000: continue
        yy, xx = np.mgrid[-R:R + 1, -R:R + 1]; w = c * np.exp(-(xx ** 2 + yy ** 2) / (2 * 4.0 ** 2))     # gaussian window against noise
        t = w.sum(); mx = (w * xx).sum() / t; my = (w * yy).sum() / t
        sxx = (w * (xx - mx) ** 2).sum() / t; syy = (w * (yy - my) ** 2).sum() / t; sxy = (w * (xx - mx) * (yy - my)).sum() / t
        tr = sxx + syy; det = sxx * syy - sxy ** 2; disc = np.sqrt(max(tr ** 2 / 4 - det, 0)); l1, l2 = tr / 2 + disc, tr / 2 - disc
        if l2 <= 0: continue
        e = 1 - np.sqrt(l2 / l1); th = 0.5 * np.degrees(np.arctan2(2 * sxy, sxx - syy))     # angle of major axis, deg from +x, y down
        rows.append((x - cx, y - cy, e, th, np.sqrt(l1)))
    a = np.array(rows); r = np.hypot(a[:, 0], a[:, 1]); ang = np.degrees(np.arctan2(a[:, 1], a[:, 0]))
    # tangential component: angle of major axis relative to the radial direction (90 = tangential)
    rel = (a[:, 3] - ang + 180) % 180
    print(f'{fn[-24:]}: {len(a)} stars  median ellipticity {np.median(a[:,2]):.3f}  median major sigma {np.median(a[:,4]):.2f} px')
    for lo, hi in ((0, 350), (350, 700), (700, 1000), (1000, 1300)):
        m = (r >= lo) & (r < hi)
        if m.sum() < 5: continue
        # mean orientation (doubled-angle average) and its coherence
        c2 = np.mean(np.exp(2j * np.radians(a[m, 3])) * a[m, 2]); ct = np.mean(np.exp(2j * np.radians(rel[m] - 90)) * a[m, 2])
        print(f'  r {lo:4d}-{hi:4d}px  n={m.sum():3d}  ellipticity {np.median(a[m,2]):.3f}  uniform-direction part {abs(c2):.3f} (axis {np.degrees(np.angle(c2))/2:6.1f} deg)  tangential part {ct.real:+.3f}')
