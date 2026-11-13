#!/usr/bin/env python3
"""Colour calibration of a linear RGB stack against Gaia DR3: the white point is a solar-type star.

    tools/gaia_colour.py STACK [--gaia CSV] [--solar-bprp 0.82] [--out gains.json]

STACK is a tools/fullres_stack.py directory (RGB.fits, 1.478"/px) or a live/restack directory (state.bin, 2.96"/px). Steps:
  1. plate-solve the stack's luminance (solve-field), for a WCS
  2. Gaia DR3 stars in the field: --gaia CSV (columns ra, dec, phot_bp_mean_mag, phot_rp_mean_mag), else the local Gaia database through PixInsight's
     Gaia process (headless, /usr/share/pixinsight/gdr3-*.xpsd)
  3. aperture photometry of every matched, unsaturated, isolated star in R, G and B (local sky annulus)
  4. fit log10(R/G) and log10(B/G) against Gaia BP-RP (robust straight lines): the measured colour response of the whole system (optics, the filter
     when fitted, the sensor's colour filters), with no assumed curves
  5. gains so that a star of BP-RP = 0.82 (the Sun) has R = G = B: gR = 10^-(aR + bR x 0.82), gB likewise, gG = 1
Why not synthetic photometry from the Gaia XP spectra: that needs the filter's transmission and the IMX178's RGB response, and neither is known (Antlia
gives no curve for the Version II blue band). Fitting against BP-RP measures the product directly. A physical system gives log(R/G) rising and
log(B/G) falling with BP-RP; the fitted slopes are reported as the check.
Writes STACK/gaia_colour.json (gains, fits, star count, residual scatter).
"""
import argparse, json, os, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)


def load_stack(d):
    from astropy.io import fits
    import gallery_export as G
    if os.path.exists(os.path.join(d, 'RGB.fits')):
        rgb = np.moveaxis(fits.getdata(os.path.join(d, 'RGB.fits')).astype(np.float32), 0, 2); scale = 1.478
    else:
        rgb, _, _ = G.read_state(os.path.join(d, 'state.bin')); scale = 2.96
    return np.nan_to_num(rgb), scale


def solve(lum, scale, work, hint=None):
    from astropy.io import fits
    from astropy.wcs import WCS
    img = os.path.join(work, 'lum.fits')
    fits.PrimaryHDU((lum - np.nanmin(lum)).astype(np.float32)).writeto(img, overwrite=True)
    cmd = ['solve-field', img, '--overwrite', '--no-plots', '--scale-units', 'arcsecperpix', '--scale-low', str(scale * 0.93), '--scale-high', str(scale * 1.07),
           '--downsample', '2' if scale < 2 else '1', '--cpulimit', '180', '--dir', work, '--new-fits', 'none', '--corr', 'none', '--rdls', 'none', '--match', 'none',
           '--index-xyls', 'none', '--solved', 'none']
    if hint: cmd += ['--ra', str(hint[0]), '--dec', str(hint[1]), '--radius', '3']
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    w = os.path.join(work, 'lum.wcs')
    return WCS(fits.getheader(w)) if os.path.exists(w) else None


def gaia_pi(ra, dec, radius, out_csv, work, mag=16.0):
    """the local Gaia DR3 database through PixInsight's Gaia process (headless; slot 7, see the PixInsight memory note)"""
    js = os.path.join(work, 'gaia.js'); log = os.path.join(work, 'gaia.log')
    open(js, 'w').write(f'''var __out = ""; Console.beginLog();
try {{ var P = new Gaia; P.dataRelease = Gaia.prototype.DataRelease_3; P.centerRA = {ra}; P.centerDec = {dec}; P.radius = {radius}; P.magnitudeLow = -1.5;
P.magnitudeHigh = {mag}; P.sortBy = Gaia.prototype.SortBy_G; P.generateTextOutput = true; P.textFormat = Gaia.prototype.TextFormat_CSV;
P.textHeaders = Gaia.prototype.TextHeaders_TableColumns; P.outputFilePath = "{out_csv}"; var ok = P.executeGlobal(); __out += "gaia " + ok + "\\n"; }}
catch (e) {{ __out += "ERR " + e + "\\n"; }}
__out += Console.endLog(); File.writeTextFile("{log}", __out);''')
    subprocess.run(['xvfb-run', '-a', '/usr/bin/PixInsight', '-n=7', '--automation-mode', '--force-exit', '-r=' + js], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600, check=False)
    return os.path.exists(out_csv)


def read_gaia(csv):
    import csv as C
    rows = []
    for r in C.DictReader(open(csv)):
        try: rows.append((float(r['ra']), float(r['dec']), float(r['phot_bp_mean_mag']), float(r['phot_rp_mean_mag']), float(r.get('phot_g_mean_mag') or 0)))
        except (ValueError, KeyError, TypeError): continue
    return np.array(rows)


def photometry(rgb, xy, r_ap, r_in, r_out):
    H, W = rgb.shape[:2]; out = []
    yy, xx = np.mgrid[-r_out:r_out + 1, -r_out:r_out + 1]; rr = np.hypot(xx, yy)
    ap, ann = rr <= r_ap, (rr >= r_in) & (rr <= r_out)
    for x, y in xy:
        xi, yi = int(round(x)), int(round(y))
        if xi < r_out or yi < r_out or xi >= W - r_out or yi >= H - r_out: out.append(None); continue
        cut = rgb[yi - r_out:yi + r_out + 1, xi - r_out:xi + r_out + 1]
        f = []
        for k in range(3):
            c = cut[..., k]; sky = np.median(c[ann]); f.append(float((c[ap] - sky).sum()))
        peak = float(cut[ap].max())
        out.append((f, peak))
    return out


def robust_line(x, y):
    keep = np.ones(len(x), bool)
    for _ in range(5):
        b, a = np.polyfit(x[keep], y[keep], 1); r = y - (a + b * x)
        s = 1.4826 * np.median(np.abs(r[keep] - np.median(r[keep])))
        keep = np.abs(r) < 3 * max(s, 1e-4)
    return a, b, float(s), int(keep.sum())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stack'); ap.add_argument('--gaia'); ap.add_argument('--solar-bprp', type=float, default=0.82); ap.add_argument('--hint', help='RA,Dec deg')
    a = ap.parse_args()
    rgb, scale = load_stack(a.stack)
    work = tempfile.mkdtemp(dir=os.path.dirname(a.stack.rstrip('/')))
    lum = rgb.mean(axis=2)
    hint = tuple(float(v) for v in a.hint.split(',')) if a.hint else None
    wcs = solve(lum, scale, work, hint)
    if wcs is None: sys.exit('the stack did not plate-solve')
    H, W = lum.shape; cra, cdec = wcs.all_pix2world([[W / 2, H / 2]], 0)[0]
    csv = a.gaia
    if not csv:
        csv = os.path.join(work, 'gaia.csv')
        if not gaia_pi(cra, cdec, 0.75 * np.hypot(W, H) * scale / 3600, csv, work): sys.exit('no Gaia stars (PixInsight query failed)')
    g = read_gaia(csv)
    px = wcs.all_world2pix(g[:, :2], 0)
    inside = (px[:, 0] > 20) & (px[:, 0] < W - 20) & (px[:, 1] > 20) & (px[:, 1] < H - 20) & np.isfinite(g[:, 2]) & np.isfinite(g[:, 3])
    g, px = g[inside], px[inside]
    # isolated: no brighter Gaia star within 8 px
    from scipy.spatial import cKDTree
    t = cKDTree(px); iso = np.array([len(t.query_ball_point(p, 8 * (1.478 / scale) * 2)) == 1 for p in px])
    g, px = g[iso], px[iso]
    r_ap = 6 if scale < 2 else 4
    ph = photometry(rgb, px, r_ap, r_ap + 4, r_ap + 9)
    sat = 0.85 * np.percentile(rgb, 99.999)
    rows = [(gg, f) for gg, p in zip(g, ph) if p is not None and p[1] < sat and min(p[0]) > 0 for f in [p[0]]]
    if len(rows) < 30: sys.exit(f'only {len(rows)} usable stars')
    bprp = np.array([r[0][2] - r[0][3] for r in rows]); F = np.array([r[1] for r in rows])
    snr_ok = F[:, 1] > np.percentile(F[:, 1], 30)                      # the brighter 70%
    bprp, F = bprp[snr_ok], F[snr_ok]
    lr, lb = np.log10(F[:, 0] / F[:, 1]), np.log10(F[:, 2] / F[:, 1])
    m = (bprp > -0.2) & (bprp < 2.5)
    ar, br, sr, nr = robust_line(bprp[m], lr[m]); ab, bb, sb, nb = robust_line(bprp[m], lb[m])
    gR, gB = 10 ** -(ar + br * a.solar_bprp), 10 ** -(ab + bb * a.solar_bprp)
    res = {'stars_matched': int(len(rows)), 'stars_fitted': [nr, nb], 'log_RG': {'a': ar, 'b_per_mag': br, 'scatter': sr}, 'log_BG': {'a': ab, 'b_per_mag': bb, 'scatter': sb},
           'gains': {'R': gR, 'G': 1.0, 'B': gB}, 'white_point': f'Gaia BP-RP = {a.solar_bprp} (solar)', 'physical': bool(br > 0 and bb < 0),
           'field_centre': [float(cra), float(cdec)], 'gaia_csv': csv}
    json.dump(res, open(os.path.join(a.stack, 'gaia_colour.json'), 'w'), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == '__main__':
    main()
