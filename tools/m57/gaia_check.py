#!/usr/bin/env python3
"""Is the deconvolved image closer to the sky than the drizzle? Measured against Gaia DR3 (ESA TAP, G < 20.5), for the drizzle and the Richardson-Lucy image of one target.

    gaia_check.py --drz drz_all.npy --rl deconv/RL28_L.npy --frames frames.txt --window 1300 0 1100 1700 --out gaia_check.json

Method (every number is computed here from the two images and the catalogue):
  * the drizzle is plate-solved with the pi_flow matcher (vote on orientation and offset, then an affine fit to Gaia); that gives the sky position of every output pixel
  * stars are detected in each image (peak > 6 sigma after smoothing, centroid in a 7 x 7 window, flux in a 6 px aperture minus an annulus), identically for both images
  * position: each image's detections are fitted to Gaia with their own affine transform (so a common offset or scale does not count), and the residual of isolated stars (no other Gaia star
    within 8 arcsec, G 12 to 17) is reported
  * photometry: instrumental magnitude against Gaia G for the same stars, slope and scatter after clipping (a measure of whether fluxes stay faithful)
  * completeness: fraction of Gaia stars with G < 18 that have a detection within 1.5 px; false detections: detections with no Gaia star (G < 20.5) within 2 px
  * pairs: Gaia pairs 3 to 7 arcsec apart (both G < 18) in which each star has its own detection within 1.5 px (resolved)
Proper motion between Gaia's 2016 epoch and the observation is ignored; it is below 0.1 arcsec for nearly all stars at this brightness.
"""
import argparse, json, os, subprocess, sys
import numpy as np
from scipy.ndimage import gaussian_filter, maximum_filter, median_filter
from scipy.spatial import cKDTree
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', 'pi_flow'))
import m57
import flow

PX = 1.478 / 2          # arcsec per output pixel


def gaia_tap(ra, dec, radius, path, gmax=20.5):
    if not os.path.exists(path):
        q = ("SELECT source_id,ra,dec,phot_g_mean_mag,bp_rp FROM gaiadr3.gaia_source WHERE 1=CONTAINS(POINT('ICRS',ra,dec),CIRCLE('ICRS',%.5f,%.5f,%.3f)) AND phot_g_mean_mag<%.1f" % (ra, dec, radius, gmax))
        subprocess.run(['curl', '-s', '-m', '300', '--data-urlencode', 'QUERY=' + q, '-d', 'REQUEST=doQuery&LANG=ADQL&FORMAT=csv', 'https://gea.esac.esa.int/tap-server/tap/sync', '-o', path], check=True)
    return np.genfromtxt(path, delimiter=',', names=True)


def detect(img, nsig=4.0):
    """peaks > nsig sigma in the smoothed image; returns x, y (centroid), flux (6 px aperture minus annulus background), significance (peak / sigma of the smoothed image)"""
    g = gaussian_filter(img, 1.5); sig = 1.4826 * np.median(abs(g - np.median(g)))
    pk = (g == maximum_filter(g, 9)) & (g > nsig * sig); ys, xs = np.nonzero(pk); H, W = img.shape; out = []
    yy, xx = np.mgrid[-14:15, -14:15]; rr = np.hypot(xx, yy)
    for y, x in zip(ys, xs):
        if y < 16 or x < 16 or y > H - 17 or x > W - 17: continue
        w = np.clip(img[y - 3:y + 4, x - 3:x + 4], 0, None); t = w.sum()
        if t <= 0: continue
        gy, gx = np.mgrid[-3:4, -3:4]; cx = x + (w * gx).sum() / t; cy = y + (w * gy).sum() / t
        cut = img[y - 14:y + 15, x - 14:x + 15]; bg = np.median(cut[(rr > 9) & (rr < 13)]); fl = (cut[rr <= 6] - bg).sum()
        out.append((cx, cy, fl, g[y, x] / sig))
    return np.array(out)


def fit_affine(det, P, gm, lo, hi, rads=(12, 6, 3, 2, 1.5)):
    """affine transform of Gaia pixel positions P (n x 2) to the detections, iterated with shrinking radius, on Gaia stars with lo < G < hi. Returns the transform (2 x 3), matched index pairs"""
    A = np.array([[1.0, 0, 0], [0, 1.0, 0]]); tree = cKDTree(det[:, :2]); sel = np.nonzero((gm > lo) & (gm < hi))[0]; Q = np.c_[P, np.ones(len(P))]
    for r in rads:
        d, j = tree.query((Q @ A.T)[sel], distance_upper_bound=r); ok = np.isfinite(d); ii, jj = sel[ok], j[ok]
        _, first = np.unique(jj, return_index=True); ii, jj = ii[first], jj[first]
        A = np.linalg.lstsq(Q[ii], det[jj, :2], rcond=None)[0].T
    return A, ii, jj


def evaluate(det, shape, P, g, iso, inwin):
    gm = g['phot_g_mean_mag']; A, ii, jj = fit_affine(det, P, gm, 12, 17); Q = np.c_[P, np.ones(len(P))]; pred = Q @ A.T; tree = cKDTree(det[:, :2])
    res = {'detections': int(len(det)), 'affine_matches_G12_17': int(len(ii))}
    keep = iso[ii]; d = (det[jj, :2] - pred[ii])[keep]; r = np.hypot(*d.T) * PX
    res['position_residual_arcsec'] = {'n_isolated': int(keep.sum()), 'rms': float(np.sqrt((r ** 2).mean())), 'median': float(np.median(r)), 'p90': float(np.percentile(r, 90))}
    # photometry on the same isolated matched stars: instrumental mag vs G
    fl = det[jj, 2][keep]; ok = fl > 0; mi = -2.5 * np.log10(fl[ok]); G = gm[ii][keep][ok]; c = np.ones(ok.sum(), bool)
    for _ in range(5):
        p = np.polyfit(G[c], mi[c], 1); rs = mi - np.polyval(p, G); s = 1.4826 * np.median(abs(rs[c] - np.median(rs[c]))); c = abs(rs) < 3 * s
    res['photometry'] = {'n': int(c.sum()), 'slope_instr_per_G': float(p[0]), 'scatter_mag_robust': float(s), 'rms_mag': float(np.sqrt((rs[c] ** 2).mean()))}
    # completeness and false detections
    d1, _ = tree.query(pred, distance_upper_bound=1.5); bright = (gm < 18) & inwin; res['completeness_G_lt_18'] = {'gaia_stars': int(bright.sum()), 'recovered_fraction': float(np.isfinite(d1[bright]).mean())}
    for lo, hi in ((12, 15), (15, 17), (17, 18)):
        m = (gm >= lo) & (gm < hi) & inwin; res['completeness_G_%d_%d' % (lo, hi)] = {'gaia_stars': int(m.sum()), 'recovered_fraction': float(np.isfinite(d1[m]).mean())}
    gt = cKDTree(pred[inwin]); dd, _ = gt.query(det[:, :2], distance_upper_bound=2.0); inside = (det[:, 0] > 60) & (det[:, 0] < shape[1] - 60) & (det[:, 1] > 60) & (det[:, 1] < shape[0] - 60)
    res['false_detections'] = {'detections_inside': int(inside.sum()), 'without_gaia_star_within_2px': int((~np.isfinite(dd) & inside).sum()), 'fraction': float((~np.isfinite(dd) & inside).sum() / max(inside.sum(), 1))}
    # resolved pairs: Gaia pairs 3-7 arcsec apart, both G < 18, each with its own nearest detection within 1.5 px, not the same detection
    idx = np.nonzero((gm < 18) & inwin)[0]; pt = cKDTree(pred[idx]); pairs = [(idx[a], idx[b]) for a, b in pt.query_pairs(7.0 / PX) if np.hypot(*(pred[idx[a]] - pred[idx[b]])) * PX >= 3.0]
    dA, jA = tree.query(pred[[p[0] for p in pairs]], distance_upper_bound=1.5) if pairs else (np.array([]), np.array([]))
    dB, jB = tree.query(pred[[p[1] for p in pairs]], distance_upper_bound=1.5) if pairs else (np.array([]), np.array([]))
    both = np.isfinite(dA) & np.isfinite(dB) & (jA != jB) if pairs else np.array([], bool)
    res['pairs_3_to_7_arcsec'] = {'gaia_pairs': len(pairs), 'resolved_fraction': float(both.mean()) if pairs else None}
    return res, A


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--drz', required=True); ap.add_argument('--rl', required=True); ap.add_argument('--frames', required=True); ap.add_argument('--window', type=int, nargs=4, required=True)
    ap.add_argument('--out', required=True); ap.add_argument('--work'); a = ap.parse_args()
    work = a.work or os.path.dirname(os.path.abspath(a.out)); os.makedirs(work, exist_ok=True); m57.set_window(*a.window)
    drz = np.load(a.drz); rgb, L, sig = m57.rgbl(drz); rl = np.load(a.rl).astype(np.float64)
    files = open(a.frames).read().split(); hdr = flow.header_of(files[len(files) // 2])
    Lp = L - np.percentile(L, 0.01) + 1.0                                   # the matcher wants a positive image (its mask is L > 0)
    wcs = flow.solve_wcs(Lp, hdr, work, 0.5)
    if wcs is None: sys.exit('plate solution of the drizzle failed')
    print('plate solution: %d Gaia stars, rms %.2f px' % (wcs['_matches'], wcs['_rms_px']))
    H, W = L.shape; cen = [(W - 1) / 2, (H - 1) / 2]; rad = 0.5 * np.hypot(W, H) * PX / 3600 * 1.05
    # centre of the window on the sky from the solution (invert world2pix by sampling the catalogue cone around the header pointing, radius generous)
    ra0, dec0 = wcs['CRVAL1'], wcs['CRVAL2']; g = gaia_tap(ra0, dec0, rad + 0.2, os.path.join(work, 'gaia_tap.csv'))
    P = np.array([flow.world2pix(wcs, r, d) for r, d in zip(g['ra'], g['dec'])]); gm = g['phot_g_mean_mag']
    inwin = (P[:, 0] > 40) & (P[:, 0] < W - 40) & (P[:, 1] > 40) & (P[:, 1] < H - 40)
    tr = cKDTree(P); d2, _ = tr.query(P, k=2); iso = d2[:, 1] * PX > 8.0                      # no other Gaia star (G < 20.5) within 8 arcsec
    sat = m57.np.load(m57.sat_path(a.drz)) if os.path.exists(m57.sat_path(a.drz)) else None
    if sat is not None:
        yy = np.clip(np.round(P[:, 1]).astype(int), 0, H - 1); xx = np.clip(np.round(P[:, 0]).astype(int), 0, W - 1)
        near = maximum_filter(sat > 0.02, size=25)[yy, xx]; inwin &= ~near                      # not on or beside a clipped star
    out = {'gaia_stars_in_window': int(inwin.sum()), 'plate_solution': {k: wcs[k] for k in ('_matches', '_rms_px')}}
    dets = {name: detect(img) for name, img in (('drizzle', L), ('deconvolved', rl))}               # all peaks above 4 sigma, with their significance
    junk = lambda d, t: evaluate(d[d[:, 3] >= t], L.shape, P, g, iso, inwin)[0]
    # 1) both images at the same significance (6 sigma): what I reported first
    out['same_threshold_6sigma'] = {n: junk(d, 6.0) for n, d in dets.items()}
    # 2) the same number of junk detections (no Gaia star within 2 px): the drizzle at 6 sigma sets the budget; the deconvolved image gets the threshold that spends the same budget
    budget = out['same_threshold_6sigma']['drizzle']['false_detections']['without_gaia_star_within_2px']; best = None
    for t in np.arange(4.0, 80.0, 0.5):
        r = junk(dets['deconvolved'], t); n = r['false_detections']['without_gaia_star_within_2px']
        if best is None or abs(n - budget) < abs(best[1] - budget): best = (t, n, r)
        if n < budget * 0.9: break
    out['matched_junk'] = {'junk_budget': budget, 'drizzle_threshold': 6.0, 'deconvolved_threshold': float(best[0]), 'drizzle': out['same_threshold_6sigma']['drizzle'], 'deconvolved': best[2]}
    # 3) the whole trade-off: recovered fraction (G < 18), resolved pairs and junk count against threshold
    curve = {}
    for n, d in dets.items():
        curve[n] = []
        for t in (4, 5, 6, 8, 10, 12, 15, 20, 30, 50):
            r = junk(d, t); curve[n].append({'threshold': t, 'detections': r['detections'], 'junk': r['false_detections']['without_gaia_star_within_2px'], 'recovered_G_lt_18': r['completeness_G_lt_18']['recovered_fraction'],
                                              'recovered_G_17_18': r['completeness_G_17_18']['recovered_fraction'], 'pairs_resolved': r['pairs_3_to_7_arcsec']['resolved_fraction'], 'position_rms_arcsec': r['position_residual_arcsec']['rms']})
    out['threshold_curve'] = curve
    json.dump(out, open(a.out, 'w'), indent=1); print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
