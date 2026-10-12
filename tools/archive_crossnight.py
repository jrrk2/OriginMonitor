#!/usr/bin/env python3
"""Can frames from different nights be registered on each other? For each target with several sessions, take the best accepted frame of each session (most stars, smallest star radius),
predict where its stars fall in the first session's frame from the two plate-solutions the Origin wrote into the headers (WCS), then check against the stars actually found: how many
land within 3 px untouched, and after a best-fit rotation + shift (found by pair voting on the star lists, no WCS used) how many agree within 1.5 px and with what rms.

    tools/archive_crossnight.py [--csv .../frames.csv] [--root /mnt/fits-backup/Astrophotography]
"""
import argparse, csv, glob, math, os, sys, collections
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS
from scipy import ndimage as ndi

def num(x):
    try: return float(x)
    except Exception: return float('nan')

def load(path):
    with fits.open(path) as h:
        hdu = h[1] if len(h) > 1 and path.endswith('.fz') else h[0]
        return hdu.data.astype(np.float32), hdu.header

def stars(d, n=150):
    """Star list in full-resolution pixel coordinates from a half-resolution mean of each 2x2 cell (Bayer colour does not matter for positions)."""
    h = d[0::2, 0::2] + d[0::2, 1::2] + d[1::2, 0::2] + d[1::2, 1::2]
    h = ndi.gaussian_filter(h - ndi.median_filter(h[::4, ::4], 15).repeat(4, 0).repeat(4, 1)[:h.shape[0], :h.shape[1]], 1.5)
    sig = 1.4826 * np.median(np.abs(h - np.median(h)))
    mx = (h == ndi.maximum_filter(h, 9)) & (h > 8 * sig)
    ys, xs = np.nonzero(mx); fl = h[ys, xs]; o = np.argsort(-fl)[:n]
    return np.c_[xs[o] * 2.0 + 0.5, ys[o] * 2.0 + 0.5]

def match(a, b, tol):
    d = np.hypot(a[:, None, 0] - b[None, :, 0], a[:, None, 1] - b[None, :, 1]); j = d.argmin(1); m = d[np.arange(len(a)), j] < tol
    return np.nonzero(m)[0], j[m]

def fit_rigid(a, b):
    """rotation + shift mapping a to b, by voting over pairs of the 40 brightest stars (scale fixed at 1)."""
    A, B = a[:40], b[:40]; votes = collections.Counter(); keep = {}
    for i in range(len(A)):
        for k in range(i + 1, len(A)):
            va = A[k] - A[i]; la = np.hypot(*va)
            if la < 150: continue
            for j in range(len(B)):
                for l in range(len(B)):
                    if j == l: continue
                    vb = B[l] - B[j]
                    if abs(np.hypot(*vb) - la) > 2.0: continue
                    th = math.atan2(vb[1], vb[0]) - math.atan2(va[1], va[0]); key = round(math.degrees(th) / 0.3)
                    votes[key] += 1; keep.setdefault(key, []).append(th)
    if not votes: return None
    th = float(np.median(keep[max(votes, key=votes.get)])); c, s = math.cos(th), math.sin(th)
    R = np.array([[c, -s], [s, c]]); P = a @ R.T
    best = None
    for i in range(min(30, len(a))):                                  # shift: vote with every candidate pairing of the brightest stars
        for j in range(min(30, len(b))):
            t = b[j] - P[i]; ai, bj = match(P + t, b, 1.5)
            if best is None or len(ai) > best[0]: best = (len(ai), t, ai, bj)
    n, t, ai, bj = best
    r = np.hypot(*(P[ai] + t - b[bj]).T); return dict(theta_deg=math.degrees(th), shift=t, inliers=int(n), rms=float(np.sqrt((r ** 2).mean())) if n else float('nan'), R=R)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--csv', default='/mnt/fits-backup/claude/archive_qc/frames.csv'); ap.add_argument('--root', default='/mnt/fits-backup/Astrophotography')
    ap.add_argument('--targets', default='Messier_101,Fireworks_Galaxy,M1_Crab,Crab_Nebula'); a = ap.parse_args()
    rows = list(csv.DictReader(open(a.csv))); by = collections.defaultdict(list)
    for r in rows:
        if r['error'] or num(r['star_radius_px']) != num(r['star_radius_px']): continue
        if r['trailed'] == 'True' or num(r['star_radius_px']) > 2.6 or num(r['stars']) < 25 or num(r['saturated_fraction']) > 0.01: continue
        by[r['session']].append(r)
    for tg in a.targets.split(','):
        ss = sorted(s for s in by if s.startswith(tg) or tg in s)
        ss = [s for s in ss if s != 'M1']
        print('\n== %s: %d sessions' % (tg, len(ss)))
        best = {}
        for s in ss:
            r = max(by[s], key=lambda r: (num(r['stars']), -num(r['star_radius_px'])))
            f = glob.glob(os.path.join(a.root, s, r['frame'])); best[s] = (r, f[0] if f else None)
        ref = None
        for s in ss:
            r, f = best[s]
            if not f: continue
            d, h = load(f); st = stars(d); w = WCS(h)
            if ref is None: ref = (s, st, w, h, r); print('  reference %s %s (%d stars, rotation %.1f deg)' % (s[:36], r['frame'], len(st), num(r['rot_deg']))); continue
            rs, rst, rw, rh, rr = ref
            try:
                world = w.pixel_to_world_values(st[:, 0], st[:, 1]); px = np.c_[rw.world_to_pixel_values(*world)]
            except Exception as e: px = None
            n0 = len(match(px, rst, 3)[0]) if px is not None else 0
            fit = fit_rigid(st, rst)
            dth = ((num(r['rot_deg']) - num(rr['rot_deg']) + 180) % 360) - 180
            print('  %-40s %-18s stars %3d  WCS prediction: %3d within 3 px | star-pair fit: rotation %.1f deg (headers say %.1f), shift %s px, %d inliers, rms %.2f px' %
                  (s[:40], r['frame'][:18], len(st), n0, fit['theta_deg'] if fit else float('nan'), dth, np.round(fit['shift']) if fit else '-', fit['inliers'] if fit else 0, fit['rms'] if fit else float('nan')))

if __name__ == '__main__':
    main()
