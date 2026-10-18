#!/usr/bin/env python3
"""Colour calibration of a drizzle against Gaia DR3 stars (broadband, no filter assumed: use a filter curve for filtered data).

    colour_cal.py --drz drz_all.npy --frames frames.txt --window 0 0 3056 2048 --out colour_cal.json [--png colour.png]

  * the drizzle (planes G, R, B) is plate-solved (pi_flow matcher) and Gaia DR3 stars with G < 15.5 are matched (brighter ones are clipped in 13 s frames; the clipped-star mask removes them)
  * every unclipped isolated star is measured in each plane (6 px aperture minus an annulus), and the instrumental colours -2.5 log10(R/G) and -2.5 log10(B/G) are fitted against Gaia BP-RP
  * the white balance makes a solar-colour star (BP-RP = 0.82) neutral; the fitted slopes say how well one gain per plane can follow the stellar colours
  * --png writes the stack with that white balance, stretched like the other pictures
"""
import argparse, json, os, sys
import numpy as np
from scipy.ndimage import gaussian_filter, maximum_filter
from scipy.spatial import cKDTree
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', 'pi_flow'))
import m57, flow
import gaia_check as gc

PX = 1.478 / 2
SOLAR = 0.82            # Gaia BP-RP of the Sun


def aperture(img, x, y, r=6, r1=9, r2=13):
    iy, ix = int(round(y)), int(round(x)); cut = img[iy - 14:iy + 15, ix - 14:ix + 15]; yy, xx = np.mgrid[-14:15, -14:15]; rr = np.hypot(xx, yy)
    return float((cut[rr <= r] - np.median(cut[(rr > r1) & (rr < r2)])).sum())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--drz', required=True); ap.add_argument('--frames', required=True); ap.add_argument('--window', type=int, nargs=4, required=True)
    ap.add_argument('--out', required=True); ap.add_argument('--png'); ap.add_argument('--gmax', type=float, default=15.5); a = ap.parse_args()
    work = os.path.dirname(os.path.abspath(a.out)); m57.set_window(*a.window)
    drz = np.load(a.drz).astype(np.float64); rgb, L, sig = m57.rgbl(drz)            # rgb planes R, G, B, background subtracted
    files = open(a.frames).read().split(); hdr = flow.header_of(files[len(files) // 2])
    wcs = flow.solve_wcs(L - np.percentile(L, 0.01) + 1.0, hdr, work, 0.5)
    if wcs is None: sys.exit('plate solution failed')
    print('plate solution: %d stars, rms %.2f px' % (wcs['_matches'], wcs['_rms_px']), flush=True)
    H, W = L.shape; rad = 0.5 * np.hypot(W, H) * PX / 3600 * 1.05
    g = gc.gaia_tap(wcs['CRVAL1'], wcs['CRVAL2'], rad + 0.2, os.path.join(work, 'gaia_tap_col.csv'), gmax=a.gmax + 0.5)
    P = np.array([flow.world2pix(wcs, r, d) for r, d in zip(g['ra'], g['dec'])]); G = g['phot_g_mean_mag']; bprp = g['bp_rp']
    ok = (G < a.gmax) & np.isfinite(bprp) & (P[:, 0] > 40) & (P[:, 0] < W - 40) & (P[:, 1] > 40) & (P[:, 1] < H - 40)
    print('Gaia stars %d; G<%.1f with colour: %d; of those inside the image: %d' % (len(g), a.gmax, ((G < a.gmax) & np.isfinite(bprp)).sum(), ok.sum()), flush=True)
    tr = cKDTree(P); d2, _ = tr.query(P, k=2); ok &= d2[:, 1] * PX > 12.0; print('after isolation: %d' % ok.sum(), flush=True)                                # isolated: nothing else (G < gmax+0.5) within 12 arcsec
    sat = np.load(m57.sat_path(a.drz)) if os.path.exists(m57.sat_path(a.drz)) else None
    if sat is not None:
        yy = np.clip(np.round(P[:, 1]).astype(int), 0, H - 1); xx = np.clip(np.round(P[:, 0]).astype(int), 0, W - 1); ok &= ~maximum_filter(sat > 0.02, size=31)[yy, xx]; print('after clipped-star mask: %d' % ok.sum(), flush=True)
    rows = []
    for i in np.nonzero(ok)[0]:
        x, y = P[i]; f = [aperture(rgb[k], x, y) for k in (0, 1, 2)]                                       # R, G, B
        if min(f) > 0: rows.append((G[i], bprp[i], f[0], f[1], f[2]))
    print('with positive flux in all three planes: %d' % len(rows), flush=True)
    if len(rows) < 10: sys.exit('too few stars')
    A = np.array(rows); print('%d stars measured (Gaia G < %.1f, isolated, unclipped)' % (len(A), a.gmax), flush=True)
    cR = -2.5 * np.log10(A[:, 2] / A[:, 3]); cB = -2.5 * np.log10(A[:, 4] / A[:, 3]); x = A[:, 1]
    out = {'stars': int(len(A)), 'plate_rms_px': wcs['_rms_px']}; gains = {}
    for name, c in (('R_over_G', cR), ('B_over_G', cB)):
        m = np.ones(len(c), bool)
        for _ in range(6):
            p = np.polyfit(x[m], c[m], 1); r = c - np.polyval(p, x); s = 1.4826 * np.median(abs(r[m] - np.median(r[m]))); m = abs(r) < 3 * s
        z = np.polyval(p, SOLAR); out[name] = {'slope_mag_per_BPRP': float(p[0]), 'intercept': float(p[1]), 'instr_colour_at_solar': float(z), 'scatter_mag': float(s), 'n': int(m.sum())}
        gains[name] = 10 ** (0.4 * z)                                                                       # multiply the plane by this so a solar star has equal flux
    out['white_balance_gains_R_G_B'] = [gains['R_over_G'], 1.0, gains['B_over_G']]
    out['note'] = 'slope = change of the instrumental colour (mag) per unit Gaia BP-RP: R/G falls and B/G rises as a star gets redder; a gain cannot change the slopes, only the zero point. Gains make a solar-colour star (BP-RP 0.82) neutral.'
    json.dump(out, open(a.out, 'w'), indent=1); print(json.dumps(out, indent=1))
    if a.png:
        from skimage import io
        w = np.array(out['white_balance_gains_R_G_B']); cal = rgb * w[:, None, None]; Lc = cal.sum(0) / 3; top = np.percentile(Lc, 99.7)
        st = lambda v: np.clip(np.arcsinh(10 * np.clip(v, 0, None) / top) / np.arcsinh(10), 0, 1)
        io.imsave(a.png, (np.stack([st(cal[k]) for k in range(3)], -1) * 255).astype(np.uint8))


if __name__ == '__main__':
    main()
