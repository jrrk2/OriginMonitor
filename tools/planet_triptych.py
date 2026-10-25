#!/usr/bin/env python3
"""Mars, Jupiter and Saturn side by side: same angular scale, north up / east left, one shared (provisional) white balance.

Each planet is an LRGB composite: colour from a per-frame interpolated (soft) colour stack, high spatial frequencies from a
luminance drizzle that uses every Bayer sample at its true position (see tools/saturn_lrgb.py for the measurement behind this).

    tools/planet_triptych.py [--out ~/Pictures/CelestronStacks/planets]
Needs build/saturn_moons/{crops.npz,calib_poly.npy} (from tools/saturn_moons.py) and the Mars/Jupiter bursts in build/planets.
"""
import argparse, glob, json, os, sys
from datetime import datetime, timedelta
import cv2, numpy as np
from scipy import ndimage as ndi
from skimage.registration import phase_cross_correlation
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import planets as P
import saturn_moons as SM

ARC = P.ARC
FOV = 33 * ARC                  # panel half-width in arcsec (+-49")

def interp_rgb(crop_minus_bl):
    """White-balanced R,G,B at full resolution from one raw Bayer crop (normalised convolution)."""
    cr = crop_minus_bl.shape[0]; eq = P.equalise(crop_minus_bl)
    planes = {'R': ((0, 1),), 'G': ((0, 0), (1, 1)), 'B': ((1, 0),)}; sig = {'R': 1.1, 'G': 0.8, 'B': 1.1}; out = []
    for c in 'RGB':
        m = np.zeros((cr, cr))
        for oy, ox in planes[c]: m[oy::2, ox::2] = 1
        out.append(ndi.gaussian_filter(eq * m, sig[c]) / np.maximum(ndi.gaussian_filter(m, sig[c]), 1e-6))
    return np.dstack(out)

def lrgb_combine(rgb_lo, y_hi, half_raw, sigma):
    """colour * (Y_hi / lowpass(Y_hi)): keeps the colour of rgb_lo, takes the fine detail from y_hi."""
    n = y_hi.shape[0]; c = rgb_lo.shape[0] // 2
    up = cv2.resize(rgb_lo[c - half_raw:c + half_raw, c - half_raw:c + half_raw].astype(np.float32), (n, n), interpolation=cv2.INTER_CUBIC)
    y_lo = 0.25 * up[..., 0] + 0.5 * up[..., 1] + 0.25 * up[..., 2]
    sh = phase_cross_correlation(y_hi / y_hi.max(), y_lo / y_lo.max(), upsample_factor=30, normalization=None)[0]
    up = np.dstack([ndi.shift(up[..., k], sh, order=3) for k in range(3)])
    yh = np.clip(y_hi, 0, None); ratio = yh / (cv2.GaussianBlur(yh, (0, 0), sigma) + 0.03 * yh.max())
    return np.clip(up * ratio[..., None], 0, None)

def north_up(img, orient):
    n = img.shape[0]; return np.dstack([SM.northup(img[..., k].astype(np.float32), orient, n / 2, n / 2, n) for k in range(3)])

def planet(pattern, exposure, offset=0.0):
    paths = sorted(sum([glob.glob(os.path.expanduser(p)) for p in pattern.split()], []))
    fr = P.load(paths, exposure); order = np.argsort([-f['q'] for f in fr]); sh = P.register(fr); k = max(3, int(round(len(fr) * 0.5))); idx = order[:k]
    rgb_lo = P.colour(fr, order, sh, 0.5)
    fl = np.median([P.plane_fluxes(f) for f in fr], 0); m = fl.mean()                      # equalise this planet's own plane fluxes
    gains = np.array([[m / fl[0], m / fl[1]], [m / fl[2], m / fl[3]]])
    y_hi = P.drizzle([fr[i] for i in idx], [sh[i] for i in idx], gains=gains)               # 3x grid, +-40 raw px
    y_hi = cv2.GaussianBlur(y_hi.astype(np.float32), (0, 0), 1.2)                           # ~0.4 raw px: suppresses the sparse-coverage drizzle pattern
    out = lrgb_combine(rgb_lo, y_hi, P.HALF, 3.3)
    orient = float(np.mean([f['orient'] for f in fr])) + offset
    return north_up(out, orient), ARC / P.S, len(fr), k, fr[len(fr) // 2]['t']

def saturn(work):
    Z = np.load(os.path.join(work, 'crops.npz')); poly = np.load(os.path.join(work, 'calib_poly.npy')); q, t = Z['q'], Z['t']
    idx = np.argsort(-q[:500])[:40]                                          # best 40 of the first ~27 min (best focus)
    imgs = []; ref = None
    for i in idx:
        rgb = interp_rgb(Z['crops'][i].astype(np.float32) - P.BL); n = rgb.shape[0]
        rgb = np.dstack([SM.northup(rgb[..., k].astype(np.float32), Z['orient'][i] + SM.corr_angle(t[i], poly), Z['cx'][i], Z['cy'][i], n) for k in range(3)])
        if ref is None: ref = rgb[..., 1]
        sh = phase_cross_correlation(ref[40:-40, 40:-40], rgb[40:-40, 40:-40, 1], upsample_factor=30, normalization=None)[0]
        imgs.append(np.dstack([ndi.shift(rgb[..., k], np.clip(sh, -3, 3), order=3) for k in range(3)]))
    rgb_lo = np.mean(imgs, 0)
    SM.G.update(crops=Z['crops'], cx=Z['cx'], cy=Z['cy'], ori=Z['orient'], t=t, q=q, poly=poly)
    NB = SM.NB; r0 = slice(NB // 2 - 24, NB // 2 + 24); win = np.hanning(48)[:, None] * np.hanning(48)[None, :]
    L = np.array([SM.northup(SM.bin2(Z['crops'][i].astype(np.float32) - SM.BL), Z['orient'][i] + SM.corr_angle(t[i], poly), Z['cx'][i] / 2, Z['cy'][i] / 2, NB) for i in idx])
    def pcc(a, b): return np.clip(phase_cross_correlation(a[r0, r0] * win, b[r0, r0] * win, upsample_factor=20, normalization=None)[0], -3, 3)
    sh = [np.zeros(2)] + [pcc(L[0], L[k]) for k in range(1, len(L))]
    for _ in range(2):
        refm = np.array([ndi.shift(L[k], sh[k], order=3) for k in range(len(L))]).mean(0); sh = [pcc(refm, L[k]) for k in range(len(L))]
    y_hi = SM.drizzle(idx, sh)                                               # 2x grid, +-60 raw px, derotated
    out = lrgb_combine(rgb_lo, y_hi, SM.HALF, 2.4)
    return out, ARC / SM.S, len(idx), 500, datetime(2026, 10, 7) + timedelta(seconds=float(np.mean(t[idx])))

def panel(rgb, px, lo_hi, label, lines, size=480):
    c = rgb.shape[0] // 2; h = int(round(FOV / px)); box = rgb[c - h:c + h, c - h:c + h]; lo, hi = lo_hi
    img = np.clip((box - lo) / (hi - lo), 0, 1) ** 0.8
    t = np.ascontiguousarray(cv2.resize((img * 255).astype(np.uint8), (size, size), interpolation=cv2.INTER_CUBIC)[..., ::-1])
    cv2.putText(t, label, (14, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2, cv2.LINE_AA)
    for j, ln in enumerate(lines): cv2.putText(t, ln, (14, 54 + 20 * j), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (190, 190, 190), 1, cv2.LINE_AA)
    L = int(20 / (2 * FOV) * size); cv2.line(t, (14, size - 24), (14 + L, size - 24), (255, 255, 255), 2); cv2.putText(t, '20"', (14, size - 32), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
    return t

def disc_stats(rgb, px, rad_arcsec=3.7):
    g = rgb[..., 1]; cy, cx = ndi.center_of_mass(g * (g > g.max() * 0.5)); yy, xx = np.mgrid[:g.shape[0], :g.shape[1]]; m = np.hypot(yy - cy, xx - cx) < rad_arcsec / px
    return [float(rgb[..., k][m].mean()) for k in range(3)]

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--out', default=os.path.expanduser('~/Pictures/CelestronStacks/planets')); ap.add_argument('--work', default='build/saturn_moons'); a = ap.parse_args()
    rot = json.load(open(os.path.join(a.out, 'jupiter_moon_match.json')))['rotation_offset_deg']
    mars, pm, nm, km, tmars = planet('build/planets/snapshot_*.fits build/planets/mars/*.fits', 0.001)
    jup, pj, nj, kj, tj = planet('build/planets/jupiter/*.fits', 0.0008, rot)
    sat, ps, ns, pool, tsat = saturn(a.work)
    stats = {n: disc_stats(r, px) for n, r, px in (('Mars', mars, pm), ('Jupiter', jup, pj), ('Saturn', sat, ps))}
    for n, (r, g, b) in stats.items(): print('%-8s R:G:B = %.2f : 1 : %.2f' % (n, r / g, b / g))
    gm, gs = (np.percentile(x[..., 1], 99.95) for x in (mars, sat))
    lim = {'Mars': (0.0, gm), 'Jupiter': (0.30 * stats['Jupiter'][1], 1.08 * max(stats['Jupiter'][:2])), 'Saturn': (0.0, gs)}
    pans = [panel(mars, pm, lim['Mars'], 'MARS', ['5.8" disc, 1.6 AU', '15 of 31 frames, 1.0 ms, ISO 100', tmars.strftime('%d %b %Y %H:%M UTC')]),
            panel(jup, pj, lim['Jupiter'], 'JUPITER', ['34" disc, 5.8 AU', '15 of 30 frames, 0.8 ms, ISO 100', tj.strftime('%d %b %Y %H:%M UTC')]),
            panel(sat, ps, lim['Saturn'], 'SATURN', ['rings nearly edge-on, 8.4 AU', 'best 40 of first 500 frames, 2 ms, ISO 200', tsat.strftime('%d %b %Y %H:%M UTC')])]
    sheet = np.hstack(pans); W = sheet.shape[1]; cap = np.zeros((70, W, 3), np.uint8)
    cv2.putText(cap, 'Same angular scale, north up / east left.  Celestron Origin 152 mm f/2.2, raw 16-bit.  LRGB: colour from the interpolated stack, detail from a drizzled luminance.  White balance provisional.', (14, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (210, 210, 210), 1, cv2.LINE_AA)
    cv2.putText(cap, 'Disc colour R:G:B  Mars %.2f:1:%.2f   Jupiter %.2f:1:%.2f   Saturn %.2f:1:%.2f' % tuple(v for n in ('Mars', 'Jupiter', 'Saturn') for v in (stats[n][0] / stats[n][1], stats[n][2] / stats[n][1])), (14, 54), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (210, 210, 210), 1, cv2.LINE_AA)
    out = os.path.join(a.out, 'triptych.png'); cv2.imwrite(out, np.vstack([sheet, cap])); print('wrote', out)

if __name__ == '__main__':
    main()
