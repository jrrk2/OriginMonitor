#!/usr/bin/env python3
"""Find the moving object in the frames of an OriginHeadless track session and measure how far it was from its predicted position.

    tools/track_check.py [DATA/track_<name>_<date>.jsonl] [--gap-min 4] [--box 60] [--search 15] [--out DIR] [--inject ARCSEC_PER_S [--inject-flux ADU]]

For every logged frame (each carries the object's predicted RA/Dec at mid-exposure and its motion):
  * plate-solve it (astrometry.net solve-field on the 2x2-binned frame, hinted by the header; the header's own position can be an arcminute out)
  * pair it with a frame of the same pointing taken about --gap-min later, cut a box around the prediction from both (the second aligned through its own
    solution) and subtract: the stars cancel, the object is a positive spot where it was and a negative one where it went
  * the positive spot within --search binned px (2.95"/px) of the prediction, its centroid through the frame's solution, is the measured position; the error is split along and across the motion
The along-track errors against time fit the frame-time convention: a constant along-track offset of rate x t means the frame time is off by t
(DATE-OBS taken as the start of the exposure; if it is the end, t comes out near -EXPTIME).

--inject tests the method without an asteroid: it adds a synthetic star moving at that rate (arcsec/s, along +RA) at each frame's predicted position.
Writes OUT/track_check.csv (one row per frame) and prints a summary. Solutions are cached in OUT/wcs/.
"""
import argparse, glob, json, math, os, subprocess, sys
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS
from scipy import ndimage

DATA = '/mnt/fits-backup/claude'


def binned(path):
    d = fits.getdata(path).astype(np.float32)
    return (d[0::2, 0::2] + d[0::2, 1::2] + d[1::2, 0::2] + d[1::2, 1::2]) / 4.0, fits.getheader(path)


def solve(path, img, hdr, wdir):
    """WCS of the binned frame (cached); None if it would not solve"""
    stem = os.path.splitext(os.path.basename(path))[0]
    wfile = os.path.join(wdir, stem + '.wcs')
    if not os.path.exists(wfile):
        tmp = os.path.join(wdir, stem + '_g.fits'); fits.writeto(tmp, img, overwrite=True)
        subprocess.run(['solve-field', tmp, '--overwrite', '--no-plots', '--new-fits', 'none', '--corr', 'none', '--match', 'none', '--rdls', 'none', '--index-xyls', 'none',
                        '--solved', 'none', '--scale-units', 'arcsecperpix', '--scale-low', '2.7', '--scale-high', '3.2', '--ra', str(hdr.get('RA', 0)), '--dec', str(hdr.get('DEC', 0)),
                        '--radius', '3', '--downsample', '2', '--cpulimit', '60', '--wcs', wfile, '--dir', wdir], capture_output=True)
        for ext in ('_g.axy', '_g-indx.xyls', '_g.match', '_g.solved', '_g.corr', '_g.rdls'):
            try: os.remove(os.path.join(wdir, stem + ext))
            except OSError: pass
        os.remove(tmp)
    return WCS(fits.getheader(wfile)) if os.path.exists(wfile) else None


def gauss(img, x, y, flux, sigma=1.3):
    yy, xx = np.mgrid[0:img.shape[0], 0:img.shape[1]]
    img += flux * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma ** 2)) / (2 * math.pi * sigma ** 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', nargs='?'); ap.add_argument('--gap-min', type=float, default=4); ap.add_argument('--box', type=int, default=60); ap.add_argument('--search', type=float, default=15)
    ap.add_argument('--out'); ap.add_argument('--inject', type=float, default=0); ap.add_argument('--inject-flux', type=float, default=40000)
    a = ap.parse_args()
    log = a.log or max(glob.glob(os.path.join(DATA, 'track_*.jsonl')), key=os.path.getmtime)
    out = a.out or os.path.splitext(log)[0] + '_check'; wdir = os.path.join(out, 'wcs'); os.makedirs(wdir, exist_ok=True)
    ev = [json.loads(l) for l in open(log) if l.strip()]
    frames = [e for e in ev if e.get('event') == 'frame' and os.path.exists(e['file'])]
    points = [e['time'] for e in ev if e.get('event') == 'point']
    def segment(e): return sum(1 for t in points if t <= e['time'])                 # frames between two re-pointings share a pointing
    def t(e): return np.datetime64(e['mid_utc'][:23])
    print('%s: %d frames, %d pointings' % (log, len(frames), len(points)))
    rows, cache = [], {}
    def load(e):
        if e['file'] not in cache:
            img, hdr = binned(e['file'])
            w = solve(e['file'], img, hdr, wdir)
            if a.inject and w is not None:                                         # a synthetic mover at the prediction, `inject` arcsec/s along +RA from the first frame
                dt = (t(e) - t(frames[0])) / np.timedelta64(1, 's')
                ra = e['ra_hours'] * 15 + a.inject * dt / 3600 / math.cos(math.radians(e['dec_deg']))
                x, y = w.all_world2pix([[ra, e['dec_deg']]], 0)[0]; gauss(img, x, y, a.inject_flux)
            cache[e['file']] = (img, w)
        return cache[e['file']]
    for i, e in enumerate(frames):
        if e.get('verdict') not in (None, 'clear', 'hazy', 'patchy', 'moon_glow'): continue
        mates = [f for f in frames if segment(f) == segment(e) and f is not e and f.get('verdict') in (None, 'clear', 'hazy', 'patchy', 'moon_glow')]
        if not mates: continue
        m = min(mates, key=lambda f: abs(abs((t(f) - t(e)) / np.timedelta64(1, 's')) - a.gap_min * 60))
        A, wa = load(e); B, wb = load(m)
        if wa is None or wb is None: rows.append({'file': e['file'], 'status': 'no solution'}); continue
        ra, dec = e['ra_hours'] * 15, e['dec_deg']
        if a.inject: ra += a.inject * ((t(e) - t(frames[0])) / np.timedelta64(1, 's')) / 3600 / math.cos(math.radians(dec))
        xa, ya = wa.all_world2pix([[ra, dec]], 0)[0]; xb, yb = wb.all_world2pix([[ra, dec]], 0)[0]
        h = a.box // 2; ia, ja, ib, jb = int(round(xa)), int(round(ya)), int(round(xb)), int(round(yb))
        if min(ia, ja, ib, jb) < h or ia + h >= A.shape[1] or ib + h >= B.shape[1] or ja + h >= A.shape[0] or jb + h >= B.shape[0]:
            rows.append({'file': e['file'], 'status': 'prediction off the frame'}); continue
        ca = A[ja - h:ja + h, ia - h:ia + h]; cb = B[jb - h:jb + h, ib - h:ib + h]
        sa, sb = ndimage.gaussian_filter(ca - np.median(ca), 1.2), ndimage.gaussian_filter(cb - np.median(cb), 1.2)
        diff = sa - sb
        noise = 1.4826 * np.median(np.abs(diff - np.median(diff)))
        # fixed stars are bright in both frames and never cancel exactly (seeing, saturation): mask them; the object is bright in one frame only
        nsa = 1.4826 * np.median(np.abs(sa - np.median(sa)))
        static = ndimage.binary_dilation(np.minimum(sa, sb) > 4 * nsa, iterations=3) | ndimage.binary_dilation((ca > 50000) | (cb > 50000), iterations=4)
        diff[static] = 0
        yy0, xx0 = np.mgrid[0:diff.shape[0], 0:diff.shape[1]]
        diff[np.hypot(xx0 - (xa - ia + h), yy0 - (ya - ja + h)) > a.search] = 0       # only near the prediction: the object, not a leftover elsewhere in the box
        # pair score: the bright spot where the object was plus the dark spot where the predicted motion puts it in the other frame (in A's pixel grid)
        gap = (t(m) - t(e)) / np.timedelta64(1, 's')
        vr0, vd0 = (a.inject, 0.0) if a.inject else (e['rate_ra_arcsec_s'], e['rate_dec_arcsec_s'])
        xm, ym = wa.all_world2pix([[ra + vr0 * gap / 3600 / math.cos(math.radians(dec)), dec + vd0 * gap / 3600]], 0)[0]
        dx, dy = int(round(xm - xa)), int(round(ym - ya))
        full = sa - sb; full[static] = 0
        neg = np.zeros_like(full); ys, xs = slice(max(0, -dy), min(full.shape[0], full.shape[0] - dy)), slice(max(0, -dx), min(full.shape[1], full.shape[1] - dx))
        neg[ys, xs] = -full[ys.start + dy:ys.stop + dy, xs.start + dx:xs.stop + dx]
        score = np.where(diff != 0, diff + neg, 0)
        py, px = np.unravel_index(np.argmax(score), score.shape); snr = score[py, px] / (noise * math.sqrt(2)) if noise > 0 else 0
        ny, nx = np.unravel_index(np.argmin(diff), diff.shape)
        y0, y1, x0, x1 = max(py - 3, 0), min(py + 4, diff.shape[0]), max(px - 3, 0), min(px + 4, diff.shape[1]); win = np.clip(diff[y0:y1, x0:x1], 0, None)
        yy, xx = np.mgrid[y0:y1, x0:x1]; cy, cx = (win * yy).sum() / win.sum(), (win * xx).sum() / win.sum()
        mra, mdec = wa.all_pix2world([[ia - h + cx, ja - h + cy]], 0)[0]
        dra = (mra - ra) * math.cos(math.radians(dec)) * 3600; ddec = (mdec - dec) * 3600
        vr, vd = (a.inject, 0.0) if a.inject else (e['rate_ra_arcsec_s'], e['rate_dec_arcsec_s']); v = math.hypot(vr, vd) or 1
        along, across = (dra * vr + ddec * vd) / v, (-dra * vd + ddec * vr) / v
        moved_px = math.hypot(nx - px, ny - py); expect_px = v * abs(gap) / 2.95
        rows.append({'file': os.path.basename(e['file']), 'status': 'ok' if snr >= 5 else 'faint', 'mid_utc': e['mid_utc'], 'mate_gap_s': round(gap), 'snr': round(snr, 1),
                     'err_ra_arcsec': round(dra, 2), 'err_dec_arcsec': round(ddec, 2), 'along_arcsec': round(along, 2), 'across_arcsec': round(across, 2),
                     'neg_spot_px': round(moved_px, 1), 'expected_px': round(expect_px, 1), 'rate_arcsec_s': round(v, 4), 'exptime': e.get('exptime')})
    import csv
    keys = sorted({k for r in rows for k in r}, key=lambda k: (k != 'file', k))
    with open(os.path.join(out, 'track_check.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, keys); w.writeheader(); w.writerows(rows)
    ok = [r for r in rows if r['status'] == 'ok']
    print('%d frames measured (%d with S/N >= 5), %d without a solution, %d off the frame' % (len(rows), len(ok), sum(r['status'] == 'no solution' for r in rows), sum(r['status'] == 'prediction off the frame' for r in rows)))
    if ok:
        md = lambda k: float(np.median([r[k] for r in ok]))
        tot = [math.hypot(r['err_ra_arcsec'], r['err_dec_arcsec']) for r in ok]
        print('error from the prediction: median %.1f" (RA %+.1f", Dec %+.1f"; along the motion %+.1f", across %+.1f"); 90%% within %.1f"' %
              (np.median(tot), md('err_ra_arcsec'), md('err_dec_arcsec'), md('along_arcsec'), md('across_arcsec'), float(np.percentile(tot, 90))))
        print('frame-time offset implied by the along-track error: %+.1f s (rate %.3f"/s)' % (md('along_arcsec') / md('rate_arcsec_s'), md('rate_arcsec_s')))
        print('negative spot %.1f px from the positive, motion predicts %.1f px' % (md('neg_spot_px'), md('expected_px')))
    print('rows:', os.path.join(out, 'track_check.csv'))


if __name__ == '__main__':
    main()
