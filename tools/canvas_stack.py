#!/usr/bin/env python3
"""Stack frames onto one fixed sky canvas, each through its own plate solution: the mosaic stacker. Field rotation never makes a ragged edge, it only varies the depth.

    tools/canvas_stack.py SOURCE [SOURCE...] [--out DIR] [--every N] [--max N] [--scale 2.95] [--centre RA,DEC] [--size WxH] [--flat auto|none|FILE]
                          [--taper 0.04] [--bg-order 1] [--clip 3] [--jobs 8] [--depth-crop 0.7]

SOURCE is a finals directory (tools/restack.py: its records.jsonl, the frames the stacker kept), a mosaic/track log (DATA/mosaic_*.jsonl: its frames with 20+ stars
and no fault verdict), a frames.tsv, a text file of FITS paths, or FITS files.
For every frame:
  * half-resolution R, G, B from the GRBG array (R=(0,1), G=mean of (0,0),(1,1), B=(1,0), as tools/moon.py), black level 4985 off, divided by the flat
    (it is fixed to the sensor, so it has to come off before the frame is turned onto the sky), scaled to ADU/s at ISO 800.
    --flat self (default) measures the vignetting from the frames: star-masked block medians of up to 60 of them, each over its own centre level, median-combined and fitted per
    plane with 1 + a r^2 + b r^4 + c r^6 plus a tilt (the tilt is sky gradient, fixed to the horizon and so to the sensor of an alt-az mount, and is not kept).
    The twilight master flats in DATA/flats (--flat master) over-correct the corners (0.7 where the frames show 0.8) and carry the twilight gradient.
  * plate-solved on the green plane within 0.5 deg of the header position (cached in OUT/wcs/), so the canvas position and turn of every pixel is the frame's own:
    first all of them in one tools/SolveFrames process (StellarSolver, after ~/stellina_processor: 136 Veil frames in 24 s, within 0.4" of solve-field at the centre),
    then solve-field -z2 for any it could not solve
  * a low-order sky surface fitted to the star-free sky and subtracted per plane (frames of different hours and pointings then meet at one level)
  * mapped onto the canvas (bilinear), weighted by 1/noise^2 and by a taper over the outer --taper of the frame, so no frame edge prints through
Two passes: the first gives each canvas pixel's weighted mean and scatter, the second keeps only samples whose green is within --clip sigma of it (satellites, planes, hot pixels;
sigma has a floor of 10% of the level, so seeing changes in star cores are not clipped). Saturated pixels keep a token weight: they only decide a pixel no frame has unsaturated.
The canvas is a TAN projection, north up, centred on the frames (or --centre) at --scale arcsec/px, sized to their union (or --size, arcmin).

Writes OUT/canvas.fits (3 planes R,G,B, linear, with the canvas WCS), OUT/depth.fits (seconds of exposure per pixel), OUT/preview.png (stretched, cropped to where the depth
reaches --depth-crop of its 95th percentile), OUT/depth.png, OUT/frames.jsonl (per frame: solved, centre, rotation, noise, weight) and OUT/summary.json.
"""
import argparse, glob, json, math, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS
from scipy import ndimage

DATA = '/mnt/fits-backup/claude'
BLACK = 4985.0
WB = (1 / 0.65, 1.0, 1 / 0.67)          # provisional white balance as tools/moon.py (R, G, B)
BAD = {'trailed', 'defocused', 'unreadable', 'no_pointing'}   # live verdicts that fault the frame itself (cloud is judged by the solve and the weights)
MIN_STARS = 20
FLATS = {'Nebula': 'master_flat_antlia_iso800_20261015_1851.fits', 'Clear': None}


def frame_list(sources, every, maxn):
    files = []
    for s in sources:
        if os.path.isdir(s) and os.path.exists(os.path.join(s, 'records.jsonl')):
            for l in open(os.path.join(s, 'records.jsonl')):
                r = json.loads(l)
                if r.get('registration', {}).get('stacked'): files.append(r['file'])
        elif s.endswith('.jsonl'):                                              # an OriginHeadless mosaic or track log: its frames, less those the live analysis faulted
            for l in open(s):
                r = json.loads(l)
                if r.get('event') == 'frame' and r.get('verdict') not in BAD and (r.get('stars') or 0) >= MIN_STARS: files.append(r['file'])
        elif s.endswith('.tsv'): files += [l.split('\t')[0] for l in open(s) if l.strip()]
        elif s.endswith(('.fits', '.fit')): files += sorted(glob.glob(s))
        else: files += [l.strip() for l in open(s) if l.strip()]
    seen = set(); files = [f for f in files if not (f in seen or seen.add(f)) and os.path.exists(f)]
    files = files[::every]
    return files[:maxn] if maxn else files


def load(path, flats):
    """R,G,B planes at half resolution in ADU/s at ISO 800 (flat-corrected), and the header"""
    h = fits.getheader(path); d = fits.getdata(path).astype(np.float32) - BLACK
    rgb = np.stack([d[0::2, 1::2], (d[0::2, 0::2] + d[1::2, 1::2]) / 2, d[1::2, 0::2]])
    flat = flats.get(str(h.get('FILTER', 'Clear')).strip())
    if flat is not None: rgb /= flat
    rgb *= np.array(WB, np.float32)[:, None, None] * (800.0 / float(h.get('ISOSPEED', 800) or 800)) / float(h.get('EXPTIME', 1) or 1)
    return rgb, h


SOLVEFRAMES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'solveframes', 'SolveFrames')


def solve_batch(files, wdir):
    """all frames without a solution through tools/SolveFrames (StellarSolver, one process: ~0.2 s a frame); whatever it cannot solve is left for solve-field"""
    todo = [f for f in files if not os.path.exists(os.path.join(wdir, os.path.splitext(os.path.basename(f))[0] + '.wcs'))]
    if not todo or not os.path.exists(SOLVEFRAMES): return
    lst = os.path.join(wdir, 'todo.txt'); open(lst, 'w').write('\n'.join(todo) + '\n')
    r = subprocess.run([SOLVEFRAMES, '--out-dir', wdir, '@' + lst], capture_output=True, text=True)
    print('  ' + r.stderr.strip().splitlines()[-1] if r.stderr.strip() else '  SolveFrames: no summary', flush=True)
    for f in todo:                                                              # a frame StellarSolver could not solve gets its chance with solve-field
        fail = os.path.join(wdir, os.path.splitext(os.path.basename(f))[0] + '.wcs.fail')
        if os.path.exists(fail): os.remove(fail)


def solve(path, wdir):
    stem = os.path.splitext(os.path.basename(path))[0]; wfile = os.path.join(wdir, stem + '.wcs')
    if not os.path.exists(wfile) and not os.path.exists(wfile + '.fail'):
        h = fits.getheader(path); d = fits.getdata(path).astype(np.float32)
        g = (d[0::2, 0::2] + d[1::2, 1::2]) / 2; tmp = os.path.join(wdir, stem + '_g.fits'); fits.writeto(tmp, g, overwrite=True)
        subprocess.run(['solve-field', tmp, '--overwrite', '--no-plots', '--new-fits', 'none', '--corr', 'none', '--match', 'none', '--rdls', 'none', '--index-xyls', 'none',
                        '--solved', 'none', '--scale-units', 'arcsecperpix', '--scale-low', '2.85', '--scale-high', '3.05', '--ra', str(h.get('RA', 0)), '--dec', str(h.get('DEC', 0)),
                        '--radius', '0.5', '--downsample', '2', '--cpulimit', '60', '--wcs', wfile, '--dir', wdir], capture_output=True)
        for ext in ('_g.axy', '_g-indx.xyls', '_g.match', '_g.solved', '_g.corr', '_g.rdls', '_g.fits'):
            try: os.remove(os.path.join(wdir, stem + ext))
            except OSError: pass
        if not os.path.exists(wfile): open(wfile + '.fail', 'w').close()
    return WCS(fits.getheader(wfile)) if os.path.exists(wfile) else None


def sky_surface(img, order, mask):
    """least-squares polynomial (order 0..2) through block medians of the unmasked sky"""
    ny, nx = img.shape; b = 32
    ys, xs, vs = [], [], []
    for y in range(0, ny - b + 1, b):
        for x in range(0, nx - b + 1, b):
            blk = img[y:y + b, x:x + b][~mask[y:y + b, x:x + b]]
            if blk.size > b * b // 3: ys.append(y + b / 2); xs.append(x + b / 2); vs.append(np.median(blk))
    ys, xs, vs = np.array(ys) / ny - 0.5, np.array(xs) / nx - 0.5, np.array(vs)
    terms = lambda X, Y: [np.ones_like(X)] + ([X, Y] if order >= 1 else []) + ([X * X, X * Y, Y * Y] if order >= 2 else [])
    keep = np.ones(len(vs), bool)
    for _ in range(3):                                                          # drop blocks of nebula or a big galaxy: they are not sky
        c, *_ = np.linalg.lstsq(np.array(terms(xs[keep], ys[keep])).T, vs[keep], rcond=None)
        res = vs - np.array(terms(xs, ys)).T @ c; s = 1.4826 * np.median(np.abs(res[keep]))
        keep = np.abs(res) < 2.5 * max(s, 1e-6)
    Y, X = np.mgrid[0:ny, 0:nx].astype(np.float32); X = X / nx - 0.5; Y = Y / ny - 0.5
    return sum(ci * t for ci, t in zip(c, terms(X, Y))).astype(np.float32)


def prepare(path, w, flats, a):
    """the sky-subtracted planes, their weight per pixel (taper / noise^2) and the frame's noise"""
    rgb, h = load(path, flats)
    g = rgb[1]; med = np.median(g); s = 1.4826 * np.median(np.abs(g - med))
    mask = ndimage.binary_dilation(g > med + 4 * s, iterations=3)                # stars and bright nebula out of the sky fit
    for k in range(3): rgb[k] -= sky_surface(rgb[k], a.bg_order, mask)
    noise = 1.4826 * float(np.median(np.abs(rgb[1][~mask] - np.median(rgb[1][~mask]))))
    ny, nx = g.shape; t = max(a.taper * min(nx, ny), 1.0)
    yy, xx = np.mgrid[0:ny, 0:nx]; edge = np.minimum(np.minimum(xx, nx - 1 - xx), np.minimum(yy, ny - 1 - yy)).astype(np.float32)
    u = np.clip(edge / t, 0, 1); taper = u * u * (3 - 2 * u)                        # smoothstep from 0 at the edge to 1 at --taper in
    sat = ndimage.binary_dilation((fits.getdata(path)[0::2, 0::2] > 60000), iterations=2)
    taper[sat] *= 1e-4                                                          # saturated cores: a level only where no frame has better
    return rgb, taper / max(noise, 1e-6) ** 2, noise, float(h.get('EXPTIME', 0) or 0), h


def cd_rotation(w):
    cd = w.wcs.cd if w.wcs.has_cd() else w.wcs.get_pc() * w.wcs.cdelt[:, None]
    return math.degrees(math.atan2(cd[1, 0], cd[1, 1])) % 360                   # angle of the frame's +y from north, east of north


def mapping(cw, fw, bbox, step=16):
    """frame pixel coordinates of every canvas pixel in bbox: exact on a coarse grid, bilinear in between"""
    x0, x1, y0, y1 = bbox
    gx = np.arange(x0, x1 + step, step); gy = np.arange(y0, y1 + step, step)
    GX, GY = np.meshgrid(gx, gy)
    ra, dec = cw.all_pix2world(GX.ravel(), GY.ravel(), 0)
    fx, fy = fw.all_world2pix(ra, dec, 0)
    fx = fx.reshape(GX.shape); fy = fy.reshape(GX.shape)
    oy, ox = np.mgrid[0:y1 - y0, 0:x1 - x0].astype(np.float32)
    cy, cx = oy / step, ox / step
    return ndimage.map_coordinates(fx, [cy, cx], order=1), ndimage.map_coordinates(fy, [cy, cx], order=1)


def footprint_bbox(cw, fw, shape, size):
    ny, nx = shape
    xs = np.r_[np.linspace(0, nx - 1, 9), np.full(9, nx - 1), np.linspace(nx - 1, 0, 9), np.zeros(9)]
    ys = np.r_[np.zeros(9), np.linspace(0, ny - 1, 9), np.full(9, ny - 1), np.linspace(ny - 1, 0, 9)]
    ra, dec = fw.all_pix2world(xs, ys, 0); cx, cy = cw.all_world2pix(ra, dec, 0)
    x0, x1 = max(int(np.floor(cx.min())), 0), min(int(np.ceil(cx.max())) + 1, size[0])
    y0, y1 = max(int(np.floor(cy.min())), 0), min(int(np.ceil(cy.max())) + 1, size[1])
    return (x0, x1, y0, y1) if x1 > x0 and y1 > y0 else None


def canvas_wcs(centre, scale, size):
    w = WCS(naxis=2); w.wcs.ctype = ['RA---TAN', 'DEC--TAN']; w.wcs.crval = list(centre)
    w.wcs.crpix = [size[0] / 2 + 0.5, size[1] / 2 + 0.5]; w.wcs.cdelt = [-scale / 3600, scale / 3600]
    return w


def stretch(img, lo=None, hi=None):
    v = img[np.isfinite(img)]
    lo = np.percentile(v, 25) if lo is None else lo; hi = np.percentile(v, 99.8) if hi is None else hi
    x = np.clip((img - lo) / max(hi - lo, 1e-9), 0, 1); return np.arcsinh(x * 30) / np.arcsinh(30)


def save_png(path, rgb):
    from PIL import Image
    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)[::-1]).save(path)     # FITS rows run bottom-up


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('sources', nargs='+'); ap.add_argument('--out'); ap.add_argument('--every', type=int, default=1); ap.add_argument('--max', type=int, default=0)
    ap.add_argument('--scale', type=float, default=2.95); ap.add_argument('--centre'); ap.add_argument('--size'); ap.add_argument('--flat', default='self')
    ap.add_argument('--taper', type=float, default=0.04); ap.add_argument('--bg-order', type=int, default=1); ap.add_argument('--clip', type=float, default=3.0)
    ap.add_argument('--jobs', type=int, default=8); ap.add_argument('--depth-crop', type=float, default=0.7)
    a = ap.parse_args()
    files = frame_list(a.sources, a.every, a.max)
    if not files: sys.exit('no frames')
    out = a.out or os.path.join(DATA, 'canvas', os.path.basename(os.path.normpath(a.sources[0])).replace('.tsv', ''))
    wdir = os.path.join(out, 'wcs'); os.makedirs(wdir, exist_ok=True)
    flats = {}
    if a.flat == 'self': pass                                                   # measured after the solve, from the frames themselves
    elif a.flat == 'master':
        for filt, name in FLATS.items():
            if name: flats[filt] = fits.getdata(os.path.join(DATA, 'flats', name)).astype(np.float32)
    elif a.flat not in ('none', 'self'):
        f = fits.getdata(a.flat).astype(np.float32); flats = {'Nebula': f, 'Clear': f}
    print('%d frames -> %s' % (len(files), out), flush=True)

    solve_batch(files, wdir)
    with ThreadPoolExecutor(a.jobs) as ex: wcss = list(ex.map(lambda f: solve(f, wdir), files))
    good = [(f, w) for f, w in zip(files, wcss) if w is not None]
    print('solved %d of %d' % (len(good), len(files)), flush=True)
    if not good: sys.exit('nothing solved')
    shape = (1024, 1528)
    if a.flat == 'self':
        v = self_flat([f for f, _ in good], shape)
        flats = {'Nebula': v, 'Clear': v}
        fits.writeto(os.path.join(out, 'self_flat.fits'), v, overwrite=True)
        print('self flat: corners/centre R %.2f G %.2f B %.2f' % tuple(float(v[k, :60, :60].mean()) for k in range(3)), flush=True)
    cen = [w.all_pix2world([[shape[1] / 2, shape[0] / 2]], 0)[0] for _, w in good]
    rots = [cd_rotation(w) for _, w in good]
    if a.centre: centre = tuple(float(v) for v in a.centre.split(','))
    else:
        ra = np.degrees(np.angle(np.mean(np.exp(1j * np.radians([c[0] for c in cen]))))) % 360
        centre = (ra, float(np.median([c[1] for c in cen])))
    if a.size: wa, ha = (float(v) for v in a.size.lower().split('x')); size = (int(wa * 60 / a.scale), int(ha * 60 / a.scale))
    else:
        big = canvas_wcs(centre, a.scale, (20000, 20000)); boxes = [footprint_bbox(big, w, shape, (20000, 20000)) for _, w in good]
        x0 = min(b[0] for b in boxes); x1 = max(b[1] for b in boxes); y0 = min(b[2] for b in boxes); y1 = max(b[3] for b in boxes)
        size = (x1 - x0 + 8, y1 - y0 + 8)
        centre = tuple(big.all_pix2world([[(x0 + x1) / 2, (y0 + y1) / 2]], 0)[0])
    cw = canvas_wcs(centre, a.scale, size)
    print('canvas %d x %d px (%.0f\' x %.0f\') at %.2f"/px, centre %.4f %+.4f; frame rotation %.1f..%.1f deg' %
          (size[0], size[1], size[0] * a.scale / 60, size[1] * a.scale / 60, a.scale, centre[0], centre[1], *rot_range(rots)), flush=True)

    S1 = np.zeros((3, size[1], size[0])); S2 = np.zeros((3, size[1], size[0])); W = np.zeros((size[1], size[0]))
    def project(f, fw):
        rgb, wt, noise, exp, h = prepare(f, fw, flats, a)
        bb = footprint_bbox(cw, fw, shape, size)
        if bb is None: return None
        fx, fy = mapping(cw, fw, bb)
        inside = (fx >= 0) & (fx <= shape[1] - 1) & (fy >= 0) & (fy <= shape[0] - 1)
        ww = np.where(inside, ndimage.map_coordinates(wt, [fy, fx], order=1, mode='constant'), 0)
        vals = np.stack([ndimage.map_coordinates(rgb[k], [fy, fx], order=1, mode='constant') for k in range(3)])
        return bb, vals, ww, noise, exp, h
    rows = {}
    def batches(items):
        with ThreadPoolExecutor(min(a.jobs, 4)) as ex:                            # numpy releases the GIL in map_coordinates; 4 keeps the memory modest
            for i in range(0, len(items), 16):
                for r in zip(items[i:i + 16], ex.map(lambda t: project(*t), items[i:i + 16])): yield r
    for n, ((f, fw), r) in enumerate(batches(good)):
        if r is None: continue
        (x0, x1, y0, y1), vals, ww, noise, exp, h = r
        S1[:, y0:y1, x0:x1] += vals * ww; S2[:, y0:y1, x0:x1] += vals * vals * ww; W[y0:y1, x0:x1] += ww
        rows[f] = {'file': f, 'noise': round(noise, 4), 'exptime': exp, 'filter': str(h.get('FILTER', '')).strip(), 'date_obs': h.get('DATE-OBS')}
        if n % 50 == 0: print('  pass 1: %d/%d' % (n + 1, len(good)), flush=True)
    with np.errstate(invalid='ignore', divide='ignore'):
        mean = S1 / W; sig = np.sqrt(np.maximum(S2 / W - mean * mean, 0))
    del S2
    if a.clip > 0:
        S1[:] = 0; W2 = np.zeros_like(W); D = np.zeros_like(W); rejected = samples = 0
        for n, ((f, fw), r) in enumerate(batches(good)):
            if r is None: continue
            (x0, x1, y0, y1), vals, ww, noise, exp, h = r
            m, s = mean[:, y0:y1, x0:x1], sig[:, y0:y1, x0:x1]
            ok = (np.abs(vals[1] - m[1]) <= a.clip * np.maximum(s[1], 0.1 * np.abs(m[1])) + 1e-9) | ~np.isfinite(m[1])
            w2 = np.where(ok, ww, 0); rejected += int((ww > 0).sum() - (w2 > 0).sum()); samples += int((ww > 0).sum())
            S1[:, y0:y1, x0:x1] += vals * w2; W2[y0:y1, x0:x1] += w2; D[y0:y1, x0:x1] += np.where(w2 > 0, exp, 0)
            if n % 50 == 0: print('  pass 2: %d/%d' % (n + 1, len(good)), flush=True)
        with np.errstate(invalid='ignore', divide='ignore'): img = S1 / W2
        depth = D; print('clipped %d of %d samples (%.2f%%)' % (rejected, samples, 100 * rejected / max(1, samples)), flush=True)
    else:
        img = mean; depth = None
    img = np.where(np.isfinite(img), img, 0).astype(np.float32)
    hdr = cw.to_header(); hdr['COMMENT'] = 'canvas_stack: planes R, G, B, linear, ADU/s at ISO 800, sky subtracted'
    fits.writeto(os.path.join(out, 'canvas.fits'), img, hdr, overwrite=True)
    if depth is not None: fits.writeto(os.path.join(out, 'depth.fits'), depth.astype(np.float32), cw.to_header(), overwrite=True)
    dep = depth if depth is not None else W
    p95 = float(np.percentile(dep[dep > 0], 95)); crop = dep >= a.depth_crop * p95
    ys, xs = np.where(crop); y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    # the sky outside the crop is left unstretched (black)
    lum = img[1][crop]; lo, hi = np.percentile(lum, 25), np.percentile(lum, 99.8)
    pv = np.stack([stretch(img[k], lo, hi) * crop for k in range(3)], -1)[y0:y1, x0:x1]
    save_png(os.path.join(out, 'preview.png'), pv)
    save_png(os.path.join(out, 'depth.png'), np.repeat((dep / max(dep.max(), 1e-9))[..., None], 3, -1))
    with open(os.path.join(out, 'frames.jsonl'), 'w') as fo:
        for (f, w), rot, c in zip(good, rots, cen):
            row = rows.get(f, {'file': f}); row.update({'rotation_deg': round(rot, 2), 'ra_deg': round(float(c[0]), 5), 'dec_deg': round(float(c[1]), 5)})
            fo.write(json.dumps(row) + '\n')
        for f in files:
            if f not in dict(good): fo.write(json.dumps({'file': f, 'solved': False}) + '\n')
    summ = {'frames': len(files), 'solved': len(good), 'canvas_px': list(size), 'scale_arcsec': a.scale, 'centre_deg': [round(centre[0], 5), round(centre[1], 5)],
            'rotation_range_deg': [round(v, 1) for v in rot_range(rots)], 'depth_s': {'max': float(dep.max()), 'p95': p95, 'median_covered': float(np.median(dep[dep > 0]))},
            'crop_px': [int(x0), int(y0), int(x1), int(y1)], 'crop_arcmin': [round((x1 - x0) * a.scale / 60, 1), round((y1 - y0) * a.scale / 60, 1)]}
    json.dump(summ, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
    print(json.dumps(summ))


def self_flat(files, shape, nmax=60, b=32):
    """vignetting per plane, from the frames: radial polynomial fitted to the median over frames of star-masked block medians, each frame over its centre level"""
    pick = files[::max(1, len(files) // nmax)][:nmax]; ny, nx = shape; gy, gx = ny // b, nx // b
    stacks = np.full((len(pick), 3, gy, gx), np.nan, np.float32)
    for i, f in enumerate(pick):
        rgb, _ = load(f, {})
        g = rgb[1]; med = np.median(g); s = 1.4826 * np.median(np.abs(g - med)); mask = ndimage.binary_dilation(g > med + 3 * s, iterations=4)
        for k in range(3):
            p = np.where(mask, np.nan, rgb[k])[:gy * b, :gx * b].reshape(gy, b, gx, b)
            bm = np.nanmedian(p, axis=(1, 3)); c = np.nanmedian(bm[gy // 2 - 2:gy // 2 + 2, gx // 2 - 2:gx // 2 + 2])
            stacks[i, k] = bm / c
    med = np.nanmedian(stacks, axis=0)
    yc, xc = (np.arange(gy) + 0.5) * b, (np.arange(gx) + 0.5) * b; X, Y = np.meshgrid((xc - nx / 2) / (nx / 2), (yc - ny / 2) / (nx / 2))
    R2 = X * X + Y * Y
    Yf, Xf = np.mgrid[0:ny, 0:nx].astype(np.float32); Xf = (Xf + 0.5 - nx / 2) / (nx / 2); Yf = (Yf + 0.5 - ny / 2) / (nx / 2); R2f = Xf * Xf + Yf * Yf
    out = np.empty((3, ny, nx), np.float32)
    for k in range(3):
        v = med[k]; ok = np.isfinite(v)
        A = np.stack([np.ones_like(X), R2, R2 ** 2, R2 ** 3, X, Y], -1)[ok]; keep = np.ones(len(A), bool)
        for _ in range(3):
            c, *_ = np.linalg.lstsq(A[keep], v[ok][keep], rcond=None); res = v[ok] - A @ c; sd = 1.4826 * np.median(np.abs(res[keep])); keep = np.abs(res) < 3 * max(sd, 1e-6)
        out[k] = (c[0] + c[1] * R2f + c[2] * R2f ** 2 + c[3] * R2f ** 3) / c[0]
    return out


def rot_range(rots):
    """smallest arc holding all rotation angles: (start, end) in degrees, end may exceed 360"""
    r = sorted(v % 360 for v in rots)
    if len(r) < 2: return (r[0], r[0]) if r else (0, 0)
    gaps = [(r[(i + 1) % len(r)] - r[i]) % 360 for i in range(len(r))]
    i = int(np.argmax(gaps)); start = r[(i + 1) % len(r)]
    return start, start + 360 - gaps[i]


if __name__ == '__main__':
    main()
