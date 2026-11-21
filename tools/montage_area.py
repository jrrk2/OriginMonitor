"""Per-visit Montage mosaic of one mosaic window, cropped and Gaia colour calibrated.  Usage: montage_area.py FRAMELOG.jsonl OUTDIR
1. frames grouped by visit (round, panel); plate solutions (StellarSolver batch, then solve-field)
2. each frame: black off, master flat, ADU/s; each visit median-combined (mean if < 5 frames) on its own grid
3. visits through mProjExec, mOverlaps, mDiffFitExec, mBgModel, mBgExec, mAdd per colour plane; one cubic gradient fit
4. crop to the largest rectangle covered by >= 2 visits (>= 1 if that keeps < 90% of the area); render
5. colour calibrated against Gaia DR3 as the live stacker calibrates a stack (LiveStacker::calibrateColour): the stars come from the local catalogue
   (tools/gaia_pack.py) with the camera signal predicted for each, a Pickles spectrum at its BP-RP through the filter and the IMX178 (tools/GaiaStars, built
   from tools/GaiaStars.pro); the gains are the median ratios of measured to predicted, the white point a G2V star
Intermediate projections are deleted at the end."""
import collections, glob, json, os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS
from scipy.spatial import cKDTree
LOG, HERE = sys.argv[1], os.path.abspath(sys.argv[2]); os.makedirs(HERE, exist_ok=True)
sys.path.insert(0, '/home/jonathan/OriginMonitor/tools'); sys.argv = sys.argv[:1]
import canvas_stack as C
GAIASTARS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'gaiastars', 'GaiaStars')
P = 'RGB'; T0 = time.time()
def log(*a): print(f'[{time.time() - T0:7.1f}s]', *a, flush=True)
def run(*cmd, cwd=HERE):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode != 0 or 'stat="ERROR"' in out: raise RuntimeError(' '.join(map(str, cmd)) + ': ' + out[-400:])
    return out

if not os.path.exists(os.path.join(HERE, 'mosaic_rgb_flat.fits')):
    visits = collections.OrderedDict()
    for l in open(LOG):
        e = json.loads(l)
        if e.get('event') == 'frame' and e.get('verdict') not in C.BAD and (e.get('stars') or 0) >= C.MIN_STARS and os.path.exists(e['file']):
            visits.setdefault(f"r{e['round']}_t{e['tile'].replace(',', '-')}", []).append(e['file'])
    files = [f for v in visits.values() for f in v]
    log(len(files), 'frames in', len(visits), 'visits')
    wdir = os.path.join(HERE, 'wcs'); os.makedirs(wdir, exist_ok=True)
    C.solve_batch(files, wdir)
    with ThreadPoolExecutor(4) as ex: ok = list(ex.map(lambda f: C.solve(f, wdir) is not None, files))
    log(sum(ok), 'of', len(files), 'solved')
    solved = {f for f, o in zip(files, ok) if o}

    flats = {k: fits.getdata(os.path.join(C.DATA, 'flats', v)).astype(np.float32) for k, v in C.FLATS.items() if v}
    def write_frame(vf):
        v, f = vf; stem = os.path.splitext(os.path.basename(f))[0]
        if all(os.path.exists(os.path.join(HERE, 'frames', v, p, stem + '.fits')) for p in P): return
        rgb, h = C.load(f, flats); wh = fits.getheader(os.path.join(wdir, stem + '.wcs')); hdr = fits.Header()
        for k in ('CRVAL1', 'CRVAL2', 'CRPIX1', 'CRPIX2', 'CD1_1', 'CD1_2', 'CD2_1', 'CD2_2'):
            if k in wh: hdr[k] = wh[k]
        hdr['CTYPE1'], hdr['CTYPE2'], hdr['EQUINOX'] = 'RA---TAN', 'DEC--TAN', 2000.0
        for k, p in enumerate(P):
            os.makedirs(os.path.join(HERE, 'frames', v, p), exist_ok=True)
            fits.writeto(os.path.join(HERE, 'frames', v, p, stem + '.fits'), rgb[k].astype(np.float32), hdr, overwrite=True)
    with ThreadPoolExecutor(4) as ex: list(ex.map(write_frame, [(v, f) for v, fs in visits.items() for f in fs if f in solved]))
    log('frames written')

    for p in P: os.makedirs(os.path.join(HERE, f'visit_{p}'), exist_ok=True)
    def combine(v):
        n = len(glob.glob(os.path.join(HERE, 'frames', v, 'G', '*.fits')))
        if n == 0: return v, 0
        d = os.path.join(HERE, 'frames', v)
        run('mImgtbl', 'G', 'g.tbl', cwd=d); run('mMakeHdr', 'g.tbl', 'visit.hdr', cwd=d)
        for p in P:
            out = os.path.join(HERE, f'visit_{p}', v + '.fits')
            if os.path.exists(out): continue
            os.makedirs(os.path.join(d, 'proj_' + p), exist_ok=True)
            run('mImgtbl', p, p + '.tbl', cwd=d)
            run('mProjExec', '-p', p, p + '.tbl', 'visit.hdr', 'proj_' + p, 'stats_' + p + '.tbl', cwd=d)
            run('mImgtbl', 'proj_' + p, 'proj_' + p + '.tbl', cwd=d)
            run('mAdd', '-a', 'median' if n >= 5 else 'mean', '-p', 'proj_' + p, 'proj_' + p + '.tbl', 'visit.hdr', out, cwd=d)
        return v, n
    with ThreadPoolExecutor(4) as ex: res = list(ex.map(combine, visits))
    log('visit images:', sum(1 for _, n in res if n), 'from', sum(n for _, n in res), 'frames')

    # 4b. coincident visits (same panel, every round) stacked into one image per panel first: with a few panels revisited
    # every round, nearly every visit overlaps every other (106 visits of 2 panels: 5567 overlaps), and mDiffFitExec's
    # memory grows with the overlaps (~60 GB per plane here).  Within a panel the visits are background matched
    # (planes, as for the mosaic) before the median, since rounds differ in sky and field rotation turns the gradients;
    # the panels are then matched as before.
    # Only for mosaics like that (more than 20 visits a panel): elsewhere the median of a panel's rotated visits leaves their edges as saw teeth.
    tiles = collections.OrderedDict()
    for v, n in res:
        if n: tiles.setdefault(v.split('_t', 1)[1], []).append(v)
    per_panel = sum(len(vs) for vs in tiles.values()) > 20 * len(tiles); src = 'tile' if per_panel else 'visit'
    for p in P: os.makedirs(os.path.join(HERE, f'tile_{p}'), exist_ok=True)
    def stack_tile(tv):
        t, vs = tv; d = os.path.join(HERE, 'tiles', t)
        for p in P:
            os.makedirs(os.path.join(d, p), exist_ok=True); os.makedirs(os.path.join(d, 'proj_' + p), exist_ok=True)
            for v in vs:
                l = os.path.join(d, p, v + '.fits')
                if not os.path.exists(l): os.symlink(os.path.join(HERE, f'visit_{p}', v + '.fits'), l)
        run('mImgtbl', 'G', 'g.tbl', cwd=d); run('mMakeHdr', 'g.tbl', 'tile.hdr', cwd=d)
        for p in P:
            out = os.path.join(HERE, f'tile_{p}', t + '.fits')
            if os.path.exists(out): continue
            run('mImgtbl', p, p + '.tbl', cwd=d)
            for x in ('diff_', 'corr_'): os.makedirs(os.path.join(d, x + p), exist_ok=True)
            run('mProjExec', '-p', p, p + '.tbl', 'tile.hdr', 'proj_' + p, 'stats_' + p + '.tbl', cwd=d)
            run('mImgtbl', 'proj_' + p, 'proj_' + p + '.tbl', cwd=d); run('mOverlaps', 'proj_' + p + '.tbl', 'diffs_' + p + '.tbl', cwd=d)
            run('mDiffFitExec', '-p', 'proj_' + p, 'diffs_' + p + '.tbl', 'tile.hdr', 'diff_' + p, 'fits_' + p + '.tbl', cwd=d)
            run('mBgModel', 'proj_' + p + '.tbl', 'fits_' + p + '.tbl', 'corrections_' + p + '.tbl', cwd=d)
            run('mBgExec', '-p', 'proj_' + p, 'proj_' + p + '.tbl', 'corrections_' + p + '.tbl', 'corr_' + p, cwd=d)
            shutil.rmtree(os.path.join(d, 'diff_' + p), ignore_errors=True)
            run('mAdd', '-a', 'median', '-p', 'corr_' + p, 'proj_' + p + '.tbl', 'tile.hdr', out, cwd=d)
        return t, len(vs)
    if per_panel:
        with ThreadPoolExecutor(2) as ex: log('panel stacks:', list(ex.map(stack_tile, tiles.items())))

    run('mImgtbl', f'{src}_G', 'visits.tbl'); run('mMakeHdr', 'visits.tbl', 'template.hdr')
    def plane(p):
        for d in ('proj', 'diff', 'corr'): os.makedirs(os.path.join(HERE, f'{d}_{p}'), exist_ok=True)
        run('mProjExec', '-p', f'{src}_{p}', 'visits.tbl', 'template.hdr', f'proj_{p}', f'stats_{p}.tbl')
        run('mImgtbl', f'proj_{p}', f'proj_{p}.tbl'); run('mOverlaps', f'proj_{p}.tbl', f'diffs_{p}.tbl')
        n = sum(1 for l in open(os.path.join(HERE, f'diffs_{p}.tbl')) if not l.startswith(('|', '\\')))
        fit = run('mDiffFitExec', '-p', f'proj_{p}', f'diffs_{p}.tbl', 'template.hdr', f'diff_{p}', f'fits_{p}.tbl')
        run('mBgModel', f'proj_{p}.tbl', f'fits_{p}.tbl', f'corrections_{p}.tbl')
        run('mBgExec', '-p', f'proj_{p}', f'proj_{p}.tbl', f'corrections_{p}.tbl', f'corr_{p}')
        run('mAdd', '-p', f'corr_{p}', f'proj_{p}.tbl', 'template.hdr', f'mosaic_{p}.fits')
        return p, n, fit[-120:]
    with ThreadPoolExecutor(3) as ex:
        for p, n, fit in ex.map(plane, P): log(p, 'overlaps', n, fit)

    img = np.stack([np.nan_to_num(fits.getdata(os.path.join(HERE, f'mosaic_{p}.fits')).astype(np.float32)) for p in P])
    hdr = fits.getheader(os.path.join(HERE, 'mosaic_G.fits'))
    area = fits.getdata(os.path.join(HERE, 'mosaic_G_area.fits')); m = area > 0
    ny, nx = m.shape; b = 48
    Y, X = np.mgrid[0:ny, 0:nx].astype(np.float32); X = X / nx - .5; Y = Y / ny - .5
    terms = lambda X, Y: [X ** i * Y ** j for i in range(4) for j in range(4 - i)]
    for k in range(3):
        ys, xs, vs = [], [], []
        for y in range(0, ny - b, b):
            for x in range(0, nx - b, b):
                if m[y:y + b, x:x + b].mean() < 0.9: continue
                vs.append(np.percentile(img[k, y:y + b, x:x + b], 30)); ys.append((y + b / 2) / ny - .5); xs.append((x + b / 2) / nx - .5)
        ys, xs, vs = map(np.array, (ys, xs, vs)); keep = np.ones(len(vs), bool)
        for _ in range(4):
            c, *_ = np.linalg.lstsq(np.array(terms(xs[keep], ys[keep])).T, vs[keep], rcond=None)
            r = vs - np.array(terms(xs, ys)).T @ c; s = 1.4826 * np.median(np.abs(r[keep])); keep = r < 2.0 * max(s, 1e-6)
        img[k] -= sum(ci * t for ci, t in zip(c, terms(X, Y)))
    del X, Y
    fits.writeto(os.path.join(HERE, 'mosaic_rgb_flat.fits'), img, hdr, overwrite=True)
    log('gradient removed', img.shape)


img = fits.getdata(os.path.join(HERE, 'mosaic_rgb_flat.fits')).astype(np.float32); hdr = fits.getheader(os.path.join(HERE, 'mosaic_rgb_flat.fits'))
area = fits.getdata(os.path.join(HERE, 'mosaic_G_area.fits')); ny, nx = area.shape
# crop
cdelt = abs(hdr['CDELT2']); depth = area / np.radians(cdelt) ** 2
def maxrect(mm):
    H, W = mm.shape; h = np.zeros(W, int); best = (0, None)
    for y in range(H):
        h = np.where(mm[y], h + 1, 0); st = []
        for x in range(W + 1):
            cur = h[x] if x < W else 0; s = x
            while st and st[-1][1] >= cur:
                s, hh = st.pop()
                if hh * (x - s) > best[0]: best = (hh * (x - s), (y - hh + 1, y + 1, s, x))
            st.append((s, cur))
    return best
f = 8; rects = {}
for need in (1, 2):
    mm = depth >= need - 0.5
    ms = mm[:ny // f * f, :nx // f * f].reshape(ny // f, f, nx // f, f).all(axis=(1, 3))
    a_, r_ = maxrect(ms); rects[need] = (a_, tuple(v * f for v in r_))
need = 2 if rects[2][0] >= 0.9 * rects[1][0] else 1
y0, y1, x0, x1 = rects[need][1]
log('crop: >=%d visits' % need, f'{x1 - x0}x{y1 - y0} px = {(x1 - x0) * cdelt:.2f} x {(y1 - y0) * cdelt:.2f} deg',
    '(>=1: %.2f x %.2f, >=2: %.2f x %.2f deg)' % tuple(v for n in (1, 2) for v in ((rects[n][1][3] - rects[n][1][2]) * cdelt, (rects[n][1][1] - rects[n][1][0]) * cdelt)))
crop = img[:, y0:y1, x0:x1]; h = hdr.copy(); h['CRPIX1'] -= x0; h['CRPIX2'] -= y0
OUT = os.path.join(HERE, 'final'); os.makedirs(OUT, exist_ok=True)
fits.writeto(os.path.join(OUT, 'crop_rgb.fits'), crop, h, overwrite=True)

# Gaia colour: the live stacker's photometry and gains (the mosaic is on the live stack's 2.95"/px). Stars G 9..13.5, BP-RP -0.2..3, no brighter star
# (G + 2.5) within 8 px, every aperture pixel covered, not near saturation; aperture 3 px, sky the median of the 6..9 px annulus
rgb = np.moveaxis(crop, 0, 2); H, W = rgb.shape[:2]; scale = cdelt * 3600; cov = depth[y0:y1, x0:x1]
wcs = WCS(h, naxis=2); cra, cdec = wcs.all_pix2world([[W / 2, H / 2]], 0)[0]
t0 = time.gmtime(os.path.getmtime(LOG)); epoch = t0.tm_year + (t0.tm_yday - 0.5) / 365.25
out = subprocess.run([GAIASTARS, str(cra), str(cdec), str(0.5 * np.hypot(W, H) * scale / 3600 + 0.05), '--filter', 'Nebula', '--epoch', str(epoch)],
                     capture_output=True, text=True, check=True).stdout.splitlines()
wl = out[0].split(','); white = np.array([float(v) for v in wl[1:4]]); model = ','.join(wl[4:])
g = np.array([[float(v) for v in l.split(',')] for l in out[2:]])
px = wcs.all_world2pix(g[:, :2], 0)
ins = (px[:, 0] >= 12) & (px[:, 0] < W - 12) & (px[:, 1] >= 12) & (px[:, 1] < H - 12)
t = cKDTree(px[ins]); gi = g[ins]; pxi = px[ins]
yy, xx = np.mgrid[-9:10, -9:10]; rr = np.hypot(xx, yy); ap, ann = rr <= 3, (rr >= 6) & (rr <= 9)
top = np.percentile(rgb.mean(2)[cov > 0.5][::3], 99.99)
rR, rB, col = [], [], []; cand = 0
for k in np.where((gi[:, 2] >= 9) & (gi[:, 2] <= 13.5) & (gi[:, 3] > -0.2) & (gi[:, 3] < 3.0))[0]:
    xi, yi = int(round(pxi[k, 0])), int(round(pxi[k, 1]))
    if cov[yi, xi] < 0.5: continue
    cand += 1
    if any(j != k and gi[j, 2] < gi[k, 2] + 2.5 for j in t.query_ball_point(pxi[k], 8)): continue
    cut = rgb[yi - 9:yi + 10, xi - 9:xi + 10]
    if (cov[yi - 3:yi + 4, xi - 3:xi + 4] < 0.5).any() or (cut.mean(2)[ap] > 0.85 * top).any(): continue
    m = [(cut[..., c][ap] - np.median(cut[..., c][ann])).sum() for c in range(3)]
    if min(m) <= 0: continue
    p = gi[k, 4:7]
    rR.append((m[1] / p[1]) / (m[0] / p[0])); rB.append((m[1] / p[1]) / (m[2] / p[2])); col.append(gi[k, 3])
rR, rB, col = map(np.array, (rR, rB, col))
mad = lambda v: 1.4826 * np.median(np.abs(v - np.median(v)))
def trend(v):
    k = np.abs(v - np.median(v)) <= 3 * mad(v)
    return float(np.polyfit(col[k], np.log10(v[k]), 1)[0])
gR, gB = np.median(rR) * white[1] / white[0], np.median(rB) * white[1] / white[2]
res = {'crop_px': [int(v) for v in (y0, y1, x0, x1)], 'min_visits': need, 'gaia_stars': int(len(gi)), 'candidates': cand, 'stars': int(len(rR)),
       'gains': [float(gR), 1.0, float(gB)], 'gain_err': [float(mad(rR) * white[1] / white[0] / np.sqrt(len(rR))), float(mad(rB) * white[1] / white[2] / np.sqrt(len(rB)))],
       'trend_r_per_bprp': trend(rR), 'trend_b_per_bprp': trend(rB), 'filter_model': model, 'catalogue': 'Gaia DR3 (ESA/Gaia/DPAC)', 'epoch': epoch}
json.dump(res, open(os.path.join(OUT, 'gaia_colour.json'), 'w'), indent=1); log('gaia', json.dumps(res))
cal = crop * np.array([gR, 1.0, gB], np.float32)[:, None, None]
fits.writeto(os.path.join(OUT, 'calibrated.fits'), cal, h, overwrite=True)
from PIL import Image
for name, c in (('uncalibrated', crop), ('calibrated', cal)):
    lum = c.mean(0); lo, hi = np.percentile(lum, 25), np.percentile(lum, 99.8)
    C.save_png(os.path.join(OUT, f'{name}.png'), np.stack([C.stretch(c[i], lo, hi) for i in range(3)], -1))
    im = Image.open(os.path.join(OUT, name + '.png')); im.thumbnail((1600, 1600)); im.save(os.path.join(OUT, name + '_small.jpg'), quality=90)

# free the intermediate projections
for d in glob.glob(os.path.join(HERE, 'frames', '*', 'proj_*')) + [os.path.join(HERE, f'{d}_{p}') for d in ('proj', 'diff', 'corr') for p in P]:
    shutil.rmtree(d, ignore_errors=True)
log('done')
