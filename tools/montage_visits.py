"""Montage mosaic of the whole 13 Nov North America window, per visit: every frame (master flat, plate-solved) is first combined with the others of its
visit (same panel, same 3 minutes, one sky level: median of 5+ frames, else mean, on the visit's own grid), then the visit images go through Montage's
global background matching (mOverlaps, mDiffFitExec, mBgModel, mBgExec) and mAdd, per colour plane; one cubic gradient fit over the whole mosaic at the end."""
import collections, glob, json, os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from astropy.io import fits
sys.path.insert(0, '/home/jonathan/OriginMonitor/tools')
sys.argv = sys.argv[:1]
import canvas_stack as C

HERE = os.path.dirname(os.path.abspath(__file__)); S = os.path.dirname(HERE)
LOG = '/mnt/fits-backup/claude/mosaic_ngc_7000_north_america_and_pelican_20261113.jsonl'
P = 'RGB'; T0 = time.time()
def log(*a): print(f'[{time.time() - T0:7.1f}s]', *a, flush=True)
def run(*cmd, cwd=HERE):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode != 0 or 'stat="ERROR"' in out: raise RuntimeError(' '.join(map(str, cmd)) + ': ' + out[-400:])
    return out

# 1. the frames, grouped by visit (round, panel)
visits = collections.OrderedDict()
for l in open(LOG):
    e = json.loads(l)
    if e.get('event') == 'frame' and e.get('verdict') not in C.BAD and (e.get('stars') or 0) >= C.MIN_STARS and os.path.exists(e['file']):
        visits.setdefault(f"r{e['round']}_t{e['tile'].replace(',', '-')}", []).append(e['file'])
files = [f for v in visits.values() for f in v]
log(len(files), 'frames in', len(visits), 'visits')

# 2. plate solutions: reuse tonight's, solve the rest (StellarSolver batch, then solve-field)
wdir = os.path.join(HERE, 'wcs'); os.makedirs(wdir, exist_ok=True)
for old in glob.glob(os.path.join(S, 'na_round1_00', 'wcs', '*.wcs')):
    if not os.path.exists(os.path.join(wdir, os.path.basename(old))): shutil.copy(old, wdir)
C.solve_batch(files, wdir)
with ThreadPoolExecutor(4) as ex: ok = list(ex.map(lambda f: C.solve(f, wdir) is not None, files))
log(sum(ok), 'of', len(files), 'solved')
solved = {f for f, o in zip(files, ok) if o}

# 3. each frame's planes (black off, master flat, ADU/s at ISO 800) with its plate solution, by visit
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
jobs = [(v, f) for v, fs in visits.items() for f in fs if f in solved]
with ThreadPoolExecutor(4) as ex: list(ex.map(write_frame, jobs))
log('frames written')

# 4. one image per visit and plane: the frames onto the visit's grid, median (5+ frames) or mean
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

# 5. Montage on the visits: projection to one header, global background matching, co-add
run('mImgtbl', 'visit_G', 'visits.tbl'); run('mMakeHdr', 'visits.tbl', 'template.hdr')
def plane(p):
    for d in ('proj', 'diff', 'corr'): os.makedirs(os.path.join(HERE, f'{d}_{p}'), exist_ok=True)
    run('mProjExec', '-p', f'visit_{p}', 'visits.tbl', 'template.hdr', f'proj_{p}', f'stats_{p}.tbl')
    run('mImgtbl', f'proj_{p}', f'proj_{p}.tbl'); run('mOverlaps', f'proj_{p}.tbl', f'diffs_{p}.tbl')
    n = sum(1 for l in open(os.path.join(HERE, f'diffs_{p}.tbl')) if not l.startswith(('|', '\\')))
    fit = run('mDiffFitExec', '-p', f'proj_{p}', f'diffs_{p}.tbl', 'template.hdr', f'diff_{p}', f'fits_{p}.tbl')
    run('mBgModel', f'proj_{p}.tbl', f'fits_{p}.tbl', f'corrections_{p}.tbl')
    run('mBgExec', '-p', f'proj_{p}', f'proj_{p}.tbl', f'corrections_{p}.tbl', f'corr_{p}')
    run('mAdd', '-p', f'corr_{p}', f'proj_{p}.tbl', 'template.hdr', f'mosaic_{p}.fits')
    return p, n, fit[-120:]
with ThreadPoolExecutor(3) as ex:
    for p, n, fit in ex.map(plane, P): log(p, 'overlaps', n, fit)

# 6. one smooth gradient off the whole mosaic per plane (the sky that all the frames shared), then a preview
img = np.stack([np.nan_to_num(fits.getdata(os.path.join(HERE, f'mosaic_{p}.fits')).astype(np.float32)) for p in P])
m = fits.getdata(os.path.join(HERE, 'mosaic_G_area.fits')) > 0
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
fits.writeto(os.path.join(HERE, 'mosaic_rgb_flat.fits'), img, fits.getheader(os.path.join(HERE, 'mosaic_G.fits')), overwrite=True)
ys_, xs_ = np.where(m); lum = img[1][m]; lo, hi = np.percentile(lum, 25), np.percentile(lum, 99.8)
pv = np.stack([C.stretch(img[k], lo, hi) * m for k in range(3)], -1)[ys_.min():ys_.max() + 1, xs_.min():xs_.max() + 1]
C.save_png(os.path.join(HERE, 'preview.png'), pv)
log('done', pv.shape)
