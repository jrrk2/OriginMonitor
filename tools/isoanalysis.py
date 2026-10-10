#!/usr/bin/env python3
"""usage: isoanalysis.py ISO:fileA:fileB ... - temporal noise from the difference of two frames at the same settings, sky level, saturation."""
import sys, numpy as np
from astropy.io import fits
from scipy import ndimage as ndi
rows = []
for arg in sys.argv[1:]:
    iso, a, b = arg.split(':')
    A = fits.getdata(a).astype(np.float64); B = fits.getdata(b).astype(np.float64); h = fits.getheader(a)
    g = lambda d: d[0::2, 0::2]                      # one Bayer plane (green on the diagonal)
    ga, gb = g(A), g(B)
    sky = ndi.median_filter(ga[::4, ::4], size=9)    # coarse local sky
    mask = np.abs(ga[::4, ::4] - sky) < 5 * np.std(ga[::4, ::4] - sky)   # source-free pixels
    d = (ga - gb)[::4, ::4]
    mean = np.median((ga + gb)[::4, ::4][mask]) / 2
    sig = np.std(d[mask]) / np.sqrt(2)               # temporal noise per pixel (ADU)
    sat = int((A >= 65000).sum()); hot = int((A >= 50000).sum())
    rows.append((int(iso), mean, sig, sat, hot, h.get('EXPTIME')))
black = 4985.0
print('ISO   exp  sky_ADU(above black)  noise_ADU  noise^2   sat_px(>=65000)  px>=50000')
for iso, mean, sig, sat, hot, e in rows:
    print(f'{iso:4d} {e}  {mean-black:10.0f} {sig:12.1f} {sig**2:10.0f} {sat:10d} {hot:12d}')
if len(rows) >= 3:       # model: noise^2 = k*sky + r^2 per ISO ... report shot-noise fraction = k*sky/noise^2 assuming Poisson with gain k=noise^2/sky at the highest sky level
    sky = np.array([m - black for _, m, *_ in rows]); n2 = np.array([s**2 for _, _, s, *_ in rows])
    A = np.c_[sky, np.ones_like(sky)]; k, r2 = np.linalg.lstsq(A, n2, rcond=None)[0]
    print(f'fit noise^2 = {k:.3f}*sky + {r2:.0f}  -> read-noise term {max(r2,0):.0f} ADU^2 (sigma {np.sqrt(max(r2,0)):.1f} ADU); shot-noise share per ISO:', [round(k*s/(k*s+max(r2,0)),2) for s in sky])
