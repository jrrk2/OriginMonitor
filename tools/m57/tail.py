#!/usr/bin/env python3
"""Camera-fixed one-sided tails on bright stars (the cable/obstruction feature): measure an empirical template in the SENSOR frame and subtract it from every raw frame before registration.

    tail.py build   --list frames.txt [--subset all|odd|even] --out tail.npz     template from isolated clipped stars of those frames
    tail.py measure --list frames.txt [--tail tail.npz] [--subset ...]           excess by angle (and the lobe centroids) in the frames, before or after the correction

The template E(q, theta) is the median, over isolated clipped stars (clipped radius <= 4 superpixels, wing level > 250 ADU), of (pixel - the star's own circular profile) / (the star's wing level)
in rings q = 0..20 superpixels (2x2 binned, 2.96 arcsec) and 36 angle bins, angle clockwise from +x (3 o'clock), y down, in the raw frame. `correct` then removes w * E(q, theta) from each star,
split between the Bayer colours in proportion to that star's own colour in its wings. The template is built per target: the tail is half as strong on M13 as on the Double Cluster.
"""
import argparse, sys
import numpy as np
from astropy.io import fits
from scipy.ndimage import label, center_of_mass, find_objects
from multiprocessing import Pool

NB, R = 36, 20
RC_MAX = 7.0           # the brightest stars (clipped radius 4 to 7 superpixels) carry the strongest spikes: they are corrected too, with the amplitude fitted for their size
QMIN = 4              # inner ring of the correction (superpixels): inside this the template is contaminated by the core
_T = {}

def _geom():
    yy, xx = np.mgrid[-R:R + 1, -R:R + 1]; r = np.hypot(xx, yy); th = np.degrees(np.arctan2(yy, xx)) % 360
    return r, np.minimum((th / (360 / NB)).astype(int), NB - 1)

def isolated_stars(d, rcmax=RC_MAX):
    """isolated clipped stars in a raw frame: (cx, cy, rc, w, excess[q, t], profile) in 2x2-binned coordinates; excess is (c - circular profile)/w"""
    H, W = d.shape[0] // 2, d.shape[1] // 2
    b = d.reshape(H, 2, W, 2).mean((1, 3)); b = b - np.median(b); lab, n = label((d >= 65535).reshape(H, 2, W, 2).any((1, 3)))
    r, tb = _geom(); ri = np.minimum(r.astype(int), R); out = []
    for k, sl in enumerate(find_objects(lab), 1):
        m = lab[sl] == k; area = int(m.sum())
        if area < 8: continue
        rc = np.sqrt(area / np.pi)
        if rc > rcmax: continue
        cy, cx = [int(round(v + s.start)) for v, s in zip(center_of_mass(m), sl)]
        if cy < R + 1 or cx < R + 1 or cy > H - R - 2 or cx > W - R - 2: continue
        if (lab[cy - R:cy + R + 1, cx - R:cx + R + 1] > 0).sum() != area: continue
        c = b[cy - R:cy + R + 1, cx - R:cx + R + 1]
        cnt = np.bincount(ri.ravel(), minlength=R + 1); prof = np.bincount(ri.ravel(), c.ravel(), minlength=R + 1) / np.maximum(cnt, 1)
        w = float(np.mean(prof[int(rc + 2):int(rc + 9)]))
        if w <= 0: continue
        e = np.bincount((ri * NB + tb).ravel(), (c - prof[ri]).ravel(), minlength=(R + 1) * NB) / np.maximum(np.bincount((ri * NB + tb).ravel(), minlength=(R + 1) * NB), 1)
        out.append((cx, cy, rc, w, (e.reshape(R + 1, NB) / w).astype(np.float32), prof))
    return out

def _read(fn): return fits.getdata(fn).astype(np.float64)
def _stamps(fn): return [s for s in isolated_stars(_read(fn))]

def collect(files, subset='all'):
    sel = [f for i, f in enumerate(files) if subset == 'all' or i % 2 == (subset == 'odd')]
    with Pool(11) as p: res = p.map(_stamps, sel)
    return [s for r in res for s in r]

RC_EDGES = np.array([0.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 7.01])

def template(stars, wmin=250.0):
    """T: median excess map; amp: median fitted amplitude of T in each star, by clipped radius (the tail grows faster than the wing level: ~0.5 T for the smallest clipped cores, ~2.5 T for the largest)"""
    s = [x for x in stars if x[3] > wmin]; E = np.array([x[4] for x in s]); rc = np.array([x[2] for x in s])
    T = np.nanmedian(E[rc <= 4.0], axis=0); T[np.isnan(T)] = 0            # shape from the stars with clipped radius <= 4 (cleanest), amplitude from all
    al = np.array([np.nansum(np.nan_to_num(e[QMIN:17]) * T[QMIN:17]) / np.sum(T[QMIN:17] ** 2) for e in E])
    amp = np.array([np.median(al[(rc >= lo) & (rc < hi)]) if ((rc >= lo) & (rc < hi)).sum() >= 15 else np.nan for lo, hi in zip(RC_EDGES[:-1], RC_EDGES[1:])])
    ctr = (RC_EDGES[:-1] + RC_EDGES[1:]) / 2; ok = np.isfinite(amp); amp = np.interp(ctr, ctr[ok], amp[ok])
    return T, amp, len(s)

def amplitude(amp, rc):
    return float(np.interp(rc, (RC_EDGES[:-1] + RC_EDGES[1:]) / 2, amp)) if amp is not None else 1.0

def interp(T, r, th):
    """template at radius r (superpixels) and angle th (deg): bilinear, periodic in angle; ring q is centred at r = q + 0.5"""
    q = np.clip(r - 0.5, 0, R - 1e-6); q0 = q.astype(int); fq = q - q0
    t = (th / (360 / NB) - 0.5) % NB; t0 = t.astype(int) % NB; t1 = (t0 + 1) % NB; ft = t - np.floor(t)
    return (T[q0, t0] * (1 - ft) + T[q0, t1] * ft) * (1 - fq) + (T[q0 + 1, t0] * (1 - ft) + T[q0 + 1, t1] * ft) * fq

def correct(d, T, amp=None, wmin=150.0):
    """subtract the template from every isolated clipped star of the raw frame d (float64, modified in place); returns d and the number of stars corrected"""
    H, W = d.shape[0] // 2, d.shape[1] // 2; cls = np.empty(d.shape, np.int8); cls[0::2, 0::2] = 0; cls[0::2, 1::2] = 1; cls[1::2, 0::2] = 2; cls[1::2, 1::2] = 3   # GRBG: G R / B G
    bg = [np.median(d[cls == k]) for k in range(4)]; n = 0
    for cx, cy, rc, w, _, _ in isolated_stars(d):
        if w < wmin: continue
        y0, x0 = 2 * (cy - R), 2 * (cx - R); box = d[y0:y0 + 2 * (2 * R + 1), x0:x0 + 2 * (2 * R + 1)]; kb = cls[y0:y0 + 2 * (2 * R + 1), x0:x0 + 2 * (2 * R + 1)]
        yy, xx = np.mgrid[0:box.shape[0], 0:box.shape[1]]; u = (xx + x0 - 0.5) / 2 - cx; v = (yy + y0 - 0.5) / 2 - cy      # superpixel coordinates of each raw pixel centre, relative to the star
        r = np.hypot(u, v); th = np.degrees(np.arctan2(v, u)) % 360
        ring = (r >= rc + 2) & (r <= rc + 9) & (box < 65535); lum = np.mean([box[ring & (kb == k)].mean() - bg[k] for k in (0, 1, 2, 3)])
        if lum <= 0: continue
        kc = np.array([(box[ring & (kb == k)].mean() - bg[k]) / lum for k in range(4)])                               # colour of this star's wings relative to the binned luminance
        tail = w * amplitude(amp, rc) * interp(T, np.minimum(r, R + 0.5 - 1e-6), th) * kc[kb]; use = (r >= max(QMIN, rc + 2)) & (r <= R) & (box < 65535)
        box[use] -= tail[use]; n += 1
    return d, n

def _excess(fn_T):
    fn, T = fn_T; d = _read(fn)
    if T is not None: correct(d, *T)
    return isolated_stars(d)

def lobes(stars, wmin=250.0, rcmax=7.0, label='', stat=np.nanmean):
    S = [s for s in stars if s[3] > wmin and s[2] <= rcmax]; E = np.array([s[4] for s in S]); A = np.nanmean(E[:, 4:11], axis=1); A = A - np.nanmedian(A, axis=1, keepdims=True)
    rng = np.random.default_rng(1); m = stat(A, 0); bs = np.array([stat(A[rng.integers(0, len(A), len(A))], 0) for _ in range(300)]); ang = np.arange(5, 360, 10); res = {}
    print('%s  %d stars' % (label, len(S))); print('angle  ' + ' '.join('%5d' % a for a in ang)); print('excess ' + ' '.join('%+5.2f' % x for x in m))
    for nm, (lo, hi) in (('lobe1 (4-5 o\'clock)', (20, 100)), ('lobe2 (7-8 o\'clock)', (110, 180))):
        k = (ang >= lo) & (ang <= hi); ww = np.clip(m[k], 0, None); cen = (ww * ang[k]).sum() / max(ww.sum(), 1e-9)
        print('  %-20s peak excess %+.3f of wing level, mean over window %+.3f, centroid %.1f deg' % (nm, m[k].max(), m[k].mean(), cen)); res[nm] = (float(m[k].max()), float(m[k].mean()), float(cen))
    print('  rms excess over all angles %.3f' % np.sqrt(np.mean(m ** 2))); return res

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter); sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('build'); p.add_argument('--list', required=True); p.add_argument('--subset', default='all', choices=['all', 'odd', 'even']); p.add_argument('--out', required=True)
    p = sp.add_parser('measure'); p.add_argument('--list', required=True); p.add_argument('--tail'); p.add_argument('--subset', default='all', choices=['all', 'odd', 'even']); p.add_argument('--median', action='store_true', help='median over stars instead of mean')
    a = ap.parse_args(); files = open(a.list).read().split()
    if a.cmd == 'build':
        T, amp, n = template(collect(files, a.subset)); np.savez(a.out, T=T, amp=amp, n=n); print('template from %d stars (%s frames); amplitude by clipped radius %s' % (n, a.subset, np.round(amp, 2)))
    else:
        z = np.load(a.tail) if a.tail else None; T = (z['T'], z['amp'] if 'amp' in z else None) if a.tail else None; sel = [f for i, f in enumerate(files) if a.subset == 'all' or i % 2 == (a.subset == 'odd')]
        with Pool(11) as p: res = p.map(_excess, [(f, T) for f in sel])
        lobes([s for r in res for s in r], label=('corrected with ' + a.tail) if a.tail else 'uncorrected', stat=np.nanmedian if a.median else np.nanmean)

if __name__ == '__main__': main()
