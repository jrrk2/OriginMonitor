#!/usr/bin/env python3
"""Is the camera's pixel noise independent from pixel to pixel? A flat-frame binning test (no slew, no mount command: only exposure/ISO settings and snapshots).

    tools/binning_test.py --out DIR [--isos 800,1600] [--levels 0.05,0.15,0.3,0.5] [--max-exposure 15]
    tools/binning_test.py --analyse DIR

The photon-transfer gain measured on 2026-10-25 and 2026-10-27 (about 0.0037 e/ADU at ISO 800: 270 ADU per electron, 240 e at full scale) is implausible for this sensor,
while raw values come in steps of about 6.4 ADU. If processing in the camera correlates neighbouring pixels, per-pixel statistics mislead. Test: at each signal level take two
identical frames A and B of a uniformly lit (twilight) sky; in each Bayer plane form the difference D = (A - k B) / sqrt(1 + k^2), k scaling B to A's level (the sky changes
between the frames), which removes the sky pattern and leaves noise. Then
  * R = 4 var(mean of 2x2 same-colour pixels) / var(one pixel): 1 for independent noise, up to 4 when neighbours share it, below 1 when they anti-correlate
  * the correlation of D between same-colour neighbours (2 raw pixels apart) and between adjacent raw pixels (different colours)
  * the gain from one pixel (signal / var) and from the 2x2 means (signal / (4 var)); they agree only when the noise is independent
  * the spacing of raw values (quantisation step) in each frame
Stars (the mount does not track before alignment, so they trail) are masked by a 5-sigma clip on D. Frames are kept in DIR.
"""
import argparse, json, os, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, '..'))
FULL = 65535.0; BLACK = 5070.0                 # black level from the 2026-10-27 ladder (all planes 5045-5166)
PLANES = {'G1': (0, 0), 'R': (0, 1), 'B': (1, 0), 'G2': (1, 1)}
Y0, Y1, X0, X1 = 200, 1000, 300, 1300          # patch (raw px, even offsets), off the frame centre


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def grab(call, port, outdir, t, iso):
    r = call(port, 120, cmd='snapshot', dir=outdir, exposure=float(t), iso=int(iso), binning=1, timeout=90)
    if not r.get('ok') or not r['path'].endswith('.fits'): raise RuntimeError('snapshot failed: %s' % r)
    return r['path']


def load(path):
    from astropy.io import fits
    return fits.getdata(path).astype(np.float64)


def level(frame):
    """median signal above black in the patch, green plane"""
    return float(np.median(frame[Y0:Y1:2, X0:X1:2])) - BLACK


def corr(a, b, m):
    a, b = a[m], b[m]
    a = a - a.mean(); b = b - b.mean()
    return float((a * b).sum() / np.sqrt((a * a).sum() * (b * b).sum()))


def analyse_pair(A, B):
    out = {}
    for name, (py, px) in PLANES.items():
        a = A[Y0 + py:Y1:2, X0 + px:X1:2] - BLACK; b = B[Y0 + py:Y1:2, X0 + px:X1:2] - BLACK
        k = np.median(a) / np.median(b)
        d = (a - k * b) / np.sqrt(1 + k * k)
        med = np.median(d); mad = 1.4826 * np.median(np.abs(d - med))
        m = np.abs(d - med) < 5 * mad
        h, w = (d.shape[0] // 2) * 2, (d.shape[1] // 2) * 2
        d2, m2 = d[:h, :w], m[:h, :w]
        mb = m2.reshape(h // 2, 2, w // 2, 2).all(axis=(1, 3))                  # 2x2 blocks with no masked pixel
        mean4 = d2.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
        v1 = float(np.var(d2[np.repeat(np.repeat(mb, 2, 0), 2, 1)]))             # same pixels as the blocks, so the two variances compare like for like
        v4 = float(np.var(mean4[mb]))
        s = float((np.median(a) + np.median(b)) / 2)
        out[name] = {'signal': s, 'var1': v1, 'var_mean4': v4, 'R': 4 * v4 / v1, 'gain1': s / v1, 'gain4': s / (4 * v4),
                     'corr_same_colour_x': corr(d[:, :-1], d[:, 1:], m[:, :-1] & m[:, 1:]), 'corr_same_colour_y': corr(d[:-1, :], d[1:, :], m[:-1, :] & m[1:, :]),
                     'masked_pct': float(100 * (1 - m.mean()))}
    # adjacent raw pixels (different colours): difference of the whole patch, each plane scaled on its own
    D = np.zeros((Y1 - Y0, X1 - X0)); M = np.zeros_like(D, dtype=bool)
    for name, (py, px) in PLANES.items():
        a = A[Y0 + py:Y1:2, X0 + px:X1:2] - BLACK; b = B[Y0 + py:Y1:2, X0 + px:X1:2] - BLACK
        k = np.median(a) / np.median(b); d = (a - k * b) / np.sqrt(1 + k * k); d = d / d.std()
        D[py::2, px::2] = d; M[py::2, px::2] = np.abs(d - np.median(d)) < 5
    out['corr_raw_x'] = corr(D[:, :-1], D[:, 1:], M[:, :-1] & M[:, 1:]); out['corr_raw_y'] = corr(D[:-1, :], D[1:, :], M[:-1, :] & M[1:, :])
    u = np.unique(A[Y0:Y1, X0:X1]); g = np.diff(u)
    out['quant_step'] = float(np.median(g[g > 0])) if g.size else None
    out['unique_values'] = int(u.size)
    return out


def summary(rows):
    lines = []
    for r in rows:
        p = r['planes']
        lines.append('ISO %4d  %6.3f s  signal %6.0f ADU (%4.1f %%)  R(G1,R,B,G2) %s  gain1 %s  gain4 %s e/ADU  corr same-colour %s  raw-adjacent x %.3f y %.3f  step %.1f ADU' % (
            r['iso'], r['exposure'], p['G1']['signal'], 100 * p['G1']['signal'] / (FULL - BLACK),
            ' '.join('%.2f' % p[n]['R'] for n in PLANES), '/'.join('%.4f' % p[n]['gain1'] for n in ('G1', 'R')), '/'.join('%.4f' % p[n]['gain4'] for n in ('G1', 'R')),
            '%.3f' % p['G1']['corr_same_colour_x'], p['corr_raw_x'], p['corr_raw_y'], p['quant_step'] or 0))
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out'); ap.add_argument('--isos', default='800,1600'); ap.add_argument('--levels', default='0.05,0.15,0.3,0.5')
    ap.add_argument('--max-exposure', type=float, default=15.0); ap.add_argument('--analyse')
    ap.add_argument('--port', type=int, default=int(os.environ.get('ORIGIN_CONTROL_PORT', 8765)))
    a = ap.parse_args()
    if a.analyse:
        recs = json.load(open(os.path.join(a.analyse, 'pairs.json')))
        for r in recs: r['planes'] = analyse_pair(load(r['a']), load(r['b']))
        json.dump(recs, open(os.path.join(a.analyse, 'binning.json'), 'w'), indent=1); print(summary(recs)); return
    if not a.out: ap.error('--out or --analyse')
    from originctl import call
    os.makedirs(a.out, exist_ok=True); recs = []
    for iso in [int(x) for x in a.isos.split(',')]:
        t = 0.2; rate = None
        for _ in range(4):                                                     # sky rate from a probe, re-taken if clipped or too faint to read
            s = level(load(grab(call, a.port, a.out, t, iso)))
            if 300 < s < 0.8 * (FULL - BLACK): rate = s / t; break
            t = t * 8 if s <= 300 else t / 8
        if rate is None: log('ISO %d: no usable probe; skipped' % iso); continue
        log('ISO %d: sky %.0f ADU/s' % (iso, rate))
        for f in [float(x) for x in a.levels.split(',')]:
            t = f * (FULL - BLACK) / rate
            if t > a.max_exposure: log('ISO %d level %.2f needs %.1f s (over --max-exposure); skipped' % (iso, f, t)); continue
            pa = grab(call, a.port, a.out, t, iso); pb = grab(call, a.port, a.out, t, iso)
            A, B = load(pa), load(pb)
            r = {'iso': iso, 'exposure': t, 'a': pa, 'b': pb, 'planes': analyse_pair(A, B)}
            rate = r['planes']['G1']['signal'] / t                              # the sky keeps darkening: aim the next level from this pair
            recs.append(r); log(summary([r]))
            json.dump([{k: v for k, v in x.items() if k != 'planes'} for x in recs], open(os.path.join(a.out, 'pairs.json'), 'w'), indent=1)
    json.dump(recs, open(os.path.join(a.out, 'binning.json'), 'w'), indent=1)
    log('summary\n' + summary(recs))


if __name__ == '__main__': main()
