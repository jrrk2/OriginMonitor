#!/usr/bin/env python3
"""Full-resolution restack of one night's raw snapshots: luminance at the sensor's full 3056 x 2048 sampling, colour as chrominance (U, V) at half resolution.

    tools/fullres_stack.py --night 2026-11-11 [--only M1,California] [--verdicts clear] [--out DATA/finals/fullres_<night>] [--jobs 8]

The live stacker (and tools/restack.py) keeps 2x2 superpixels, 2.96"/px. This rebuilds from the raw GRBG snapshots instead:

  * frames: focus_log.csv rows of the night (noon to noon UTC) with the given live verdicts, matched to their snapshot_*.fits (tools/restack.py frames/group)
  * fixed pattern: per-pixel median of all the night's frames (each less its sky level; different fields, so no star stays put), high-passed, subtracted from
    every raw frame (it removed faint diagonal walking-noise streaks on M1; split-half correlation 0.81 on 11 Nov)
  * each frame: edge-aware debayer (OpenCV, GRBG = COLOR_BayerGB2RGB_EA) to full-resolution RGB; a sky plane (fitted to 64 px block medians, nebula rejected) removed per channel;
    Y = 0.299 R + 0.587 G + 0.114 B at full resolution, U = 0.492 (B - Y) and V = 0.877 (R - Y) binned 2x2 (chrominance carries far less detail than luminance
    and the Bayer mosaic samples R and B only once per 2x2 cell anyway)
  * registration: stars (local maxima of the smoothed Y) matched to the reference frame's with astroalign, then a similarity transform (rotation, scale, shift)
    fitted to all matched pairs; the alt-az mount rotates the field through the night. Frames with fewer than 25 matched stars or a fit rms over 0.6 px are left out
  * reference: the frame with the most stars among those of lowest HFR (from the live log)
  * stacking: two passes; pass 1 per-pixel mean and standard deviation, pass 2 the weighted mean of values within 3 sigma (hot pixels, satellites, aircraft);
    weight = 1 / (background noise of Y)^2
Writes OUT/<target>/: Y.fits (float32, full res), U.fits and V.fits (half res), RGB.fits (full res, U/V upsampled), coverage.fits, preview.png (asinh stretch),
frames.jsonl (per frame: stars, matched, rms, rotation, noise, used) and manifest.json.
"""
import argparse, json, math, os, sys, time
from concurrent.futures import ProcessPoolExecutor
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import restack                                                  # noqa: E402

W, H = 3056, 2048


PATTERN = None                                                  # sensor fixed pattern (raw ADU, Bayer), set in each worker


FLUX_REF = 6400.0                                               # exposure x ISO every frame is scaled to (8 s at ISO 800)


def flux_scale(path):
    h = read_raw(path)[1]
    return FLUX_REF / max(1e-6, float(h.get('EXPTIME', 8) or 8) * float(h.get('ISOSPEED', 800) or 800))


def frame_format(h):
    """our daemon's snapshots: GRBG. The Origin app's own FITS (Light000xx.fits.fz, BAYERPAT = RGGB): RGGB as written, NOT flipped (12 Nov 2026, built
    straight from the raw data: a 23 Jan frame matches a November snapshot on 46 stars at 0.14 px unflipped, and on none flipped)"""
    if str(h.get('BAYERPAT', '')).strip().upper() == 'RGGB': return 'app'
    return 'snapshot'


def read_raw(path):
    from astropy.io import fits
    with fits.open(path) as hl:
        hdu = next(x for x in hl if x.data is not None)
        return hdu.data.astype(np.float32), hdu.header


def load(path, pattern=None):
    import cv2
    raw, h = read_raw(path)
    fmt = frame_format(h)
    scale = FLUX_REF / max(1e-6, float(h.get('EXPTIME', 8) or 8) * float(h.get('ISOSPEED', 800) or 800))
    pat = pattern if pattern is not None else PATTERN
    if pat is not None and fmt == 'snapshot': raw -= pat                       # the fixed pattern is of our snapshots' camera mode only
    raw = np.clip(raw + 2048, 0, 65535).astype(np.uint16)          # keep a pedestal so the subtraction cannot clip the sky at 0
    rgb = cv2.cvtColor(raw, cv2.COLOR_BayerBG2RGB_EA if fmt == 'app' else cv2.COLOR_BayerGB2RGB_EA).astype(np.float32)
    rgb *= scale
    for c in range(3): rgb[..., c] -= background(rgb[..., c])
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = bin2(0.492 * (b - y)); v = bin2(0.877 * (r - y))
    return y, u, v


def pattern_from(paths, out):
    """fixed pattern of the sensor (hot/warm pixels, column and row structure) from the night's own frames: the per-pixel median of the frames, each less its own
    per-Bayer-channel sky level, over frames of different fields so no star stays on one pixel; its large-scale part (vignetting, glow) is left to the per-frame
    background fit"""
    from astropy.io import fits
    import cv2
    stack = np.empty((len(paths), H, W), np.float16)
    for k, p in enumerate(paths):
        a = fits.getdata(p).astype(np.float32)
        for i in range(2):
            for j in range(2): a[i::2, j::2] -= np.median(a[i::2, j::2][::4, ::4])
        stack[k] = a
    pat = np.empty((H, W), np.float32)
    for y0 in range(0, H, 128): pat[y0:y0 + 128] = np.median(stack[:, y0:y0 + 128].astype(np.float32), axis=0)
    for i in range(2):
        for j in range(2):
            ch = pat[i::2, j::2]; pat[i::2, j::2] = ch - cv2.blur(cv2.medianBlur(ch, 5), (31, 31))
    fits.PrimaryHDU(pat).writeto(out, overwrite=True)
    return pat


def init_worker(pattern_file):
    global PATTERN
    from astropy.io import fits
    PATTERN = fits.getdata(pattern_file).astype(np.float32) if pattern_file else None


def bin2(a):
    return a.reshape(a.shape[0] // 2, 2, a.shape[1] // 2, 2).mean(axis=(1, 3))


def background(ch, block=64):
    """sky as a plane fitted to the 64 px block medians (block medians more than 2 sigma above the plane, i.e. nebula, rejected and refitted): a plane keeps
    nebulosity that fills the field (a block-median model took the California Nebula's broad glow away and left its filaments)"""
    h, w = ch.shape; bh, bw = h // block, w // block
    m = np.median(ch[:bh * block, :bw * block].reshape(bh, block, bw, block).transpose(0, 2, 1, 3).reshape(bh, bw, -1), axis=2).ravel()
    gy, gx = np.mgrid[0:bh, 0:bw]; gx = (gx.ravel() + 0.5) * block; gy = (gy.ravel() + 0.5) * block
    A = np.column_stack([np.ones_like(gx), gx, gy]); keep = np.ones(len(m), bool)
    for _ in range(4):
        c, *_ = np.linalg.lstsq(A[keep], m[keep], rcond=None); r = m - A @ c
        sd = 1.4826 * np.median(np.abs(r[keep] - np.median(r[keep]))) or 1.0
        keep = (r < 2 * sd) & (r > -4 * sd)
    # the plane sits on the lower part of the block distribution; its intercept is set to the 20th percentile of the residuals so nebulous fields keep their glow
    c[0] += np.percentile(r, 20)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    return (c[0] + c[1] * xx + c[2] * yy).astype(np.float32)


def noise(y):
    """pixel-to-pixel noise: robust sigma of the image less its 5 px median (nebulosity and gradients, which the plane background now keeps, do not count)"""
    import cv2
    s = np.nan_to_num(np.asarray(y, np.float32)[::2, ::2]); hp = s - cv2.medianBlur(s, 5)
    return 1.4826 * float(np.median(np.abs(hp - np.median(hp))))


def stars(y, n=150, satlevel=None):
    """centroids (x, y) of the brightest n isolated star peaks"""
    import cv2
    from scipy import ndimage
    sm = cv2.GaussianBlur(y, (0, 0), 1.5); sig = noise(sm) or 1.0
    if satlevel is None: satlevel = 0.9 * 65535                      # pass 0.9 x 65535 x the frame's flux scale for scaled frames (flux_scale)
    peaks = (sm == ndimage.maximum_filter(sm, size=9)) & (sm > 8 * sig)
    peaks[:16, :] = peaks[-16:, :] = False; peaks[:, :16] = peaks[:, -16:] = False
    yy, xx = np.nonzero(peaks)
    if len(xx) == 0: return np.zeros((0, 2))
    order = np.argsort(-sm[yy, xx])
    out = []
    for i in order:
        x0, y0 = xx[i], yy[i]
        if sm[y0, x0] > satlevel: continue
        cut = y[y0 - 3:y0 + 4, x0 - 3:x0 + 4]; cut = np.clip(cut - np.median(cut), 0, None); t = cut.sum()
        if t <= 0: continue
        gy, gx = np.mgrid[-3:4, -3:4]
        out.append((x0 + (gx * cut).sum() / t, y0 + (gy * cut).sum() / t))
        if len(out) >= n: break
    return np.array(out)


def register(src, ref):
    """2x3 similarity matrix taking frame pixels to reference pixels, with diagnostics"""
    import astroalign as aa, cv2
    if len(src) < 25: return None, {'error': 'few stars'}
    try: _, (ms, mr) = aa.find_transform(src, ref, max_control_points=60)
    except Exception as e: return None, {'error': str(e)[:80]}
    M, inl = cv2.estimateAffinePartial2D(ms.astype(np.float32), mr.astype(np.float32), method=cv2.RANSAC, ransacReprojThreshold=1.5)
    if M is None: return None, {'error': 'no fit'}
    k = inl.ravel().astype(bool)
    pred = ms[k] @ M[:, :2].T + M[:, 2]; rms = float(np.sqrt(np.mean(np.sum((pred - mr[k]) ** 2, axis=1))))
    return M, {'matched': int(k.sum()), 'rms_px': round(rms, 3), 'rot_deg': round(math.degrees(math.atan2(M[1, 0], M[0, 0])), 3),
               'scale': round(math.hypot(M[0, 0], M[1, 0]), 5), 'dx': round(float(M[0, 2]), 2), 'dy': round(float(M[1, 2]), 2)}


def warp(y, u, v, M):
    import cv2
    Mh = M.copy(); Mh[:, 2] = (M[:, 2] + 0.5) / 2 - 0.5               # the same transform on the 2x2-binned grid
    f = dict(flags=cv2.INTER_LANCZOS4, borderMode=cv2.BORDER_CONSTANT, borderValue=np.nan)
    return (cv2.warpAffine(y, M, (W, H), **f), cv2.warpAffine(u, Mh, (W // 2, H // 2), **f), cv2.warpAffine(v, Mh, (W // 2, H // 2), **f))


# ---- workers (process pool; the reference star list is passed in) ----
def measure(args):
    path, ref = args
    y, _, _ = load(path); s = stars(y, satlevel=0.9 * 65535 * flux_scale(path)); M, info = register(s, ref)
    info.update(file=path, stars=len(s), noise=round(noise(y), 3))
    if M is not None: info['M'] = M.tolist()
    return info


def accumulate(args):
    """pass 1 (stats None): sums for mean/std; pass 2: weighted clipped sums"""
    items, stats, kappa = args
    acc = None
    for path, M, w in items:
        planes = warp(*load(path), np.array(M))
        if acc is None: acc = [[np.zeros(p.shape, np.float64) for _ in range(3)] for p in planes]
        for i, p in enumerate(planes):
            ok = np.isfinite(p); p = np.where(ok, p, 0.0)
            if stats is None:
                acc[i][0] += ok; acc[i][1] += p; acc[i][2] += p.astype(np.float64) ** 2
            else:
                mean, sd = stats[i]
                ok &= np.abs(p - mean) <= kappa * np.maximum(sd, 1e-3)
                acc[i][0] += ok * w; acc[i][1] += np.where(ok, p, 0.0) * w; acc[i][2] += ok
    return acc


def add(a, b):
    if a is None: return b
    if b is None: return a
    for i in range(3):
        for j in range(3): a[i][j] += b[i][j]
    return a


def write(path, data, hdr=None):
    from astropy.io import fits
    fits.PrimaryHDU(np.asarray(data, np.float32), header=hdr).writeto(path, overwrite=True)


def preview(rgb, path):
    import cv2
    out = np.empty_like(rgb)
    y = rgb.mean(axis=2); fin = np.isfinite(y)
    lo = np.percentile(y[fin], 25); hi = np.percentile(y[fin], 99.95)
    s = (hi - lo) / 400.0                                   # asinh softening
    for c in range(3): out[..., c] = np.arcsinh(np.clip(rgb[..., c] - lo, 0, None) / s) / np.arcsinh((hi - lo) / s)
    out = np.nan_to_num(np.clip(out, 0, 1))
    cv2.imwrite(path, (out[..., ::-1] * 65535).astype(np.uint16))


def run(g, out, jobs, kappa, pattern_file=None, ref_file=None):
    t0 = time.time(); d = os.path.join(out, restack.safe(g['name'])); os.makedirs(d, exist_ok=True)
    rs = g['rows']
    cands = [r for r in rs if r['stars'] >= 100 and r['hfr'] > 0] or rs
    ref = {'file': ref_file, 'hfr': 0} if ref_file else min(cands, key=lambda r: r['hfr'])
    from astropy.io import fits as _f
    ry, _, _ = load(ref['file'], _f.getdata(pattern_file).astype(np.float32) if pattern_file else None); rstars = stars(ry, satlevel=0.9 * 65535 * flux_scale(ref["file"]))
    print(f"{g['name']}: {len(rs)} frames, reference {os.path.basename(ref['file'])} ({len(rstars)} stars, HFR {ref['hfr']})", flush=True)
    with ProcessPoolExecutor(jobs, initializer=init_worker, initargs=(pattern_file,)) as ex: infos = list(ex.map(measure, [(r['file'], rstars) for r in rs], chunksize=4))
    for r, info in zip(rs, infos): info['utc'] = r['utc'].isoformat(); info['verdict'] = r.get('verdict')
    # frames that do not match the reference directly (another night, pointed and rotated differently: little of the field in common among the 150
    # brightest stars) are matched to an anchor frame of their own session (folder), and the anchor once to the reference with 300 stars: M = M_anchor . M_frame
    reffmt = frame_format(read_raw(ref['file'])[1])
    for k, i in enumerate(infos):            # a frame stored with the other parity cannot be matched to the reference by a similarity: any "match" would be false
        if frame_format(read_raw(rs[k]['file'])[1]) != reffmt: i.pop('M', None); i['error'] = 'other parity: through its session anchor'
    okf = lambda i: 'M' in i and i.get('matched', 0) >= 25 and i.get('rms_px', 9) <= 0.6
    import collections
    sessions = collections.defaultdict(list)
    for k, i in enumerate(infos):
        if not okf(i): sessions[os.path.dirname(rs[k]['file'])].append(k)
    for sess, ks in sessions.items():
        allk = [k for k, r in enumerate(rs) if os.path.dirname(r['file']) == sess]
        if len(ks) < 5: continue
        Ma = None; rsN = {n: stars(ry, n=n, satlevel=0.9 * 65535 * flux_scale(ref['file'])) for n in (300, 500)}
        samefmt = frame_format(read_raw(rs[allk[0]]['file'])[1]) == reffmt
        for frac in ((0.5, 0.25, 0.75, 0.1, 0.9, 0.4, 0.6) if samefmt else ()):       # star bridging only for the same parity; the others go through the sky
            anchor = rs[allk[min(len(allk) - 1, int(frac * len(allk)))]]['file']
            ay, _, _ = load(anchor); sat = 0.9 * 65535 * flux_scale(anchor)
            for n in (300, 500):
                asN = stars(ay, n=n, satlevel=sat)
                for _ in range(2):
                    Ma, ia = register(asN, rsN[n])
                    if Ma is not None and ia.get('matched', 0) >= 25 and ia.get('rms_px', 9) <= 0.6: break
                    Ma = None
                if Ma is not None: break
            if Ma is not None: break
        if Ma is None:          # stars alone are too few in common between nights: go through the sky (both plate-solved)
            work = os.path.join(out, 'solve_tmp'); os.makedirs(work, exist_ok=True)
            if not hasattr(run, '_refwcs'): run._refwcs = solve_wcs(ry, work)
            anchor = rs[allk[len(allk) // 2]]['file']; ay, _, _ = load(anchor); sat = 0.9 * 65535 * flux_scale(anchor)
            hdr = read_raw(anchor)[1]; hint = (hdr.get('RA'), hdr.get('DEC')) if hdr.get('RA') is not None else None
            wa = solve_wcs(ay, work, hint)
            if wa is not None and run._refwcs is not None:
                Ma = wcs_transform(wa, run._refwcs); ia = {'matched': 'wcs', 'rms_px': 'wcs'}
        if Ma is None: print(f"  session {os.path.basename(sess)}: its anchor does not match the reference (stars or plate solve); left out", flush=True); continue
        astars = stars(ay, satlevel=sat)
        with ProcessPoolExecutor(jobs, initializer=init_worker, initargs=(pattern_file,)) as ex: sub = list(ex.map(measure, [(rs[k]['file'], astars) for k in ks], chunksize=4))
        A = np.vstack([Ma, [0, 0, 1]]); joined = 0
        for k, si in zip(ks, sub):
            if 'M' not in si or si.get('matched', 0) < 25 or si.get('rms_px', 9) > 0.6: continue
            Mc = (A @ np.vstack([np.array(si['M']), [0, 0, 1]]))[:2]
            si.update(M=Mc.tolist(), via_anchor=os.path.basename(anchor), rot_deg=round(float(np.degrees(np.arctan2(Mc[1, 0], Mc[0, 0]))), 3), scale=round(float(np.hypot(Mc[0, 0], Mc[1, 0])), 5),
                      dx=round(float(Mc[0, 2]), 2), dy=round(float(Mc[1, 2]), 2), utc=infos[k].get('utc'), verdict=infos[k].get('verdict'))
            infos[k] = si; joined += 1
        print(f"  session {os.path.basename(sess)}: anchor matched the reference ({ia['matched']} stars, {ia['rms_px']} px); {joined}/{len(ks)} frames joined through it", flush=True)
    good = [i for i in infos if 'M' in i and i['matched'] >= 25 and i['rms_px'] <= 0.6 and 0.995 < i['scale'] < 1.005]
    nz = np.median([i['noise'] for i in good]); good = [i for i in good if i['noise'] < 2.0 * nz]      # a frame twice as noisy as the typical one adds little
    for i in good: i['weight'] = round((nz / i['noise']) ** 2, 4)
    used = {i['file'] for i in good}
    for i in infos: i['used'] = i['file'] in used
    print(f"  registered {len(good)}/{len(infos)} in {time.time() - t0:.0f} s", flush=True)
    items = [(i['file'], i['M'], i['weight']) for i in good]; chunks = [items[k::jobs] for k in range(jobs)]
    stats = None; acc = None
    with ProcessPoolExecutor(jobs, initializer=init_worker, initargs=(pattern_file,)) as ex:
        for part in ex.map(accumulate, [(c, None, kappa) for c in chunks]): acc = add(acc, part)
    stats = []
    for n, s, ss in acc:
        n = np.maximum(n, 1); m = s / n; stats.append((m.astype(np.float32), np.sqrt(np.maximum(ss / n - m * m, 0)).astype(np.float32)))
    print(f"  pass 1 done {time.time() - t0:.0f} s", flush=True)
    acc = None
    with ProcessPoolExecutor(jobs, initializer=init_worker, initargs=(pattern_file,)) as ex:
        for part in ex.map(accumulate, [(c, stats, kappa) for c in chunks]): acc = add(acc, part)
    res = []
    for wsum, s, n in acc: res.append((np.where(wsum > 0, s / np.maximum(wsum, 1e-9), np.nan), n))
    (Y, nY), (U, _), (V, _) = res
    import cv2
    Uf = cv2.resize(U.astype(np.float32), (W, H), interpolation=cv2.INTER_CUBIC); Vf = cv2.resize(V.astype(np.float32), (W, H), interpolation=cv2.INTER_CUBIC)
    R = Y + Vf / 0.877; B = Y + Uf / 0.492; G = (Y - 0.299 * R - 0.114 * B) / 0.587
    rgb = np.dstack([R, G, B])
    exp = sum(r['exp'] for r in rs if r['file'] in used)
    from astropy.io import fits
    h = fits.Header(); h['OBJECT'] = g['name']; h['FILTER'] = g['filter']; h['NCOMBINE'] = len(good); h['EXPTIME'] = exp; h['REFFRAME'] = os.path.basename(ref['file'])
    h['PIXSCALE'] = (1.478, 'arcsec/px (Y; U,V twice that)')
    write(os.path.join(d, 'Y.fits'), Y, h); write(os.path.join(d, 'U.fits'), U, h); write(os.path.join(d, 'V.fits'), V, h)
    write(os.path.join(d, 'RGB.fits'), np.moveaxis(rgb, 2, 0), h); write(os.path.join(d, 'coverage.fits'), nY)
    preview(rgb, os.path.join(d, 'preview.png'))
    with open(os.path.join(d, 'frames.jsonl'), 'w') as f:
        for i in infos: f.write(json.dumps({k: v for k, v in i.items() if k != 'M'}) + '\n')
    core = nY[H // 4:3 * H // 4, W // 4:3 * W // 4]
    ysd = noise(Y[H // 4:3 * H // 4, W // 4:3 * W // 4])
    man = {'name': g['name'], 'filter': g['filter'], 'frames_offered': len(rs), 'frames_stacked': len(good), 'exposure_s': exp, 'reference': ref['file'],
           'median_frame_noise_adu': round(float(nz), 2), 'stack_noise_adu': round(ysd, 3), 'noise_gain': round(float(nz) / ysd, 1) if ysd else None,
           'median_rms_px': round(float(np.median([i['rms_px'] for i in good])), 3), 'rotation_range_deg': [min(i['rot_deg'] for i in good), max(i['rot_deg'] for i in good)],
           'centre_coverage_median': int(np.median(core)), 'seconds': round(time.time() - t0)}
    json.dump(man, open(os.path.join(d, 'manifest.json'), 'w'), indent=1)
    print('  ' + json.dumps(man), flush=True)
    return man


def solve_wcs(y, work, hint=None):
    """plate-solve a luminance image (solve-field, scale 1.35-1.6"/px, hinted when RA/Dec are known); the astropy WCS, or None"""
    import subprocess, tempfile
    from astropy.io import fits
    from astropy.wcs import WCS
    d = tempfile.mkdtemp(dir=work); img = os.path.join(d, 'y.fits')
    fits.PrimaryHDU(np.nan_to_num(y - np.nanmin(y)).astype(np.float32)).writeto(img, overwrite=True)
    cmd = ['solve-field', img, '--overwrite', '--no-plots', '--scale-units', 'arcsecperpix', '--scale-low', '1.35', '--scale-high', '1.6', '--downsample', '2',
           '--cpulimit', '120', '--dir', d, '--new-fits', 'none', '--corr', 'none', '--rdls', 'none', '--match', 'none', '--index-xyls', 'none', '--solved', 'none']
    if hint: cmd += ['--ra', str(hint[0]), '--dec', str(hint[1]), '--radius', '3']
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    w = os.path.join(d, 'y.wcs')
    return WCS(fits.getheader(w)) if os.path.exists(w) else None


def wcs_transform(wa, wb):
    """2x3 affine taking pixels of image A to image B through the sky (a grid of points over A). Full affine, not a similarity: the Origin app's frames are
    mirrored relative to our snapshots (the plate solves' CD determinants have opposite signs, 12 Nov 2026), and only an affine can carry the reflection"""
    import cv2
    gy, gx = np.mgrid[100:H - 100:12j, 100:W - 100:16j]; pa = np.column_stack([gx.ravel(), gy.ravel()])
    sky = wa.all_pix2world(pa, 0); pb = wb.all_world2pix(sky, 0)
    ok = np.all(np.isfinite(pb), axis=1) & (pb[:, 0] > -W) & (pb[:, 0] < 2 * W) & (pb[:, 1] > -H) & (pb[:, 1] < 2 * H)
    M, _ = cv2.estimateAffine2D(pa[ok].astype(np.float32), pb[ok].astype(np.float32))
    return M


def combine(dirs, colour, out):
    """luminance from several stacks on one grid, each scaled to the first (median ratio over pixels well above the sky) and weighted by 1/noise^2;
    colour from one stack's U, V (scaled with its Y)"""
    import cv2
    from astropy.io import fits
    Ys = [fits.getdata(os.path.join(d, 'Y.fits')).astype(np.float32) for d in dirs]
    ref = Ys[0]; fin = np.isfinite(ref)
    smooth = lambda a: cv2.GaussianBlur(np.nan_to_num(a), (0, 0), 3)
    sr = smooth(ref); info = []
    num = np.zeros_like(ref, np.float64); den = np.zeros_like(ref, np.float64)
    for d, Y in zip(dirs, Ys):
        sy = smooth(Y); m = np.isfinite(Y) & fin & (sr > np.percentile(sr[fin], 99)) & (sy > 0)
        k = float(np.median(sr[m] / sy[m])) if m.sum() > 100 else 1.0
        Yk = Y * k; nz = noise(Yk[H // 4:3 * H // 4, W // 4:3 * W // 4]); w = 1.0 / max(nz, 1e-6) ** 2
        ok = np.isfinite(Yk); num += np.where(ok, Yk, 0) * w; den += ok * w
        info.append({'stack': d, 'scale_to_first': round(k, 4), 'noise': round(nz, 3), 'weight': w})
    Y = np.where(den > 0, num / np.maximum(den, 1e-30), np.nan).astype(np.float32)
    cdir = colour or dirs[0]; kc = next(i['scale_to_first'] for i in info if i['stack'] == cdir)
    U = fits.getdata(os.path.join(cdir, 'U.fits')).astype(np.float32) * kc; V = fits.getdata(os.path.join(cdir, 'V.fits')).astype(np.float32) * kc
    Uf = cv2.resize(np.nan_to_num(U), (W, H), interpolation=cv2.INTER_CUBIC); Vf = cv2.resize(np.nan_to_num(V), (W, H), interpolation=cv2.INTER_CUBIC)
    R = Y + Vf / 0.877; B = Y + Uf / 0.492; G = (Y - 0.299 * R - 0.114 * B) / 0.587
    rgb = np.dstack([R, G, B])
    os.makedirs(out, exist_ok=True)
    write(os.path.join(out, 'Y.fits'), Y); write(os.path.join(out, 'RGB.fits'), np.moveaxis(rgb, 2, 0)); preview(rgb, os.path.join(out, 'preview.png'))
    ysd = noise(Y[H // 4:3 * H // 4, W // 4:3 * W // 4])
    man = {'combined': info, 'colour_from': cdir, 'stack_noise_adu': round(ysd, 3)}
    json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=1); print(json.dumps(man), flush=True)
    return man


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', default=os.environ.get('ORIGIN_DATA_DIR', '/mnt/fits-backup/claude'))
    ap.add_argument('--night', help='evening date, YYYY-MM-DD (frames from noon that day to noon the next, UTC)')
    ap.add_argument('--files', help='@LIST (one path per line) or comma-separated FITS/.fz files to stack as one target, instead of --night (any source: our snapshots or the Origin app\'s Light frames)')
    ap.add_argument('--name', default='stack'); ap.add_argument('--ref', help='reference frame (e.g. another stack\'s, so the results share a grid)')
    ap.add_argument('--combine', help='DIR_A,DIR_B[,...]: noise-weighted luminance of these stacks (same grid), each scaled to the first by a fit over the frame; colour (U, V) from --colour')
    ap.add_argument('--colour', help='--combine: the stack whose U and V give the colour (default the first)')
    ap.add_argument('--only', default=''); ap.add_argument('--verdicts', default='clear'); ap.add_argument('--out')
    ap.add_argument('--jobs', type=int, default=8); ap.add_argument('--kappa', type=float, default=3.0); ap.add_argument('--min-frames', type=int, default=20)
    ap.add_argument('--no-pattern', action='store_true', help='skip the fixed-pattern frame built from the night')
    a = ap.parse_args()
    if a.combine: return combine(a.combine.split(','), a.colour, a.out)
    if a.files:
        from astropy.io import fits
        from datetime import datetime, timezone
        paths = [l.strip() for l in open(a.files[1:])] if a.files.startswith('@') else a.files.split(',')
        rows = []
        for pth in (x for x in paths if x):
            h = read_raw(pth)[1]
            try: t = datetime.fromisoformat(str(h.get('DATE-OBS'))).replace(tzinfo=timezone.utc)
            except ValueError: t = datetime.now(timezone.utc)
            rows.append({'file': pth, 'exp': float(h.get('EXPTIME', 0) or 0), 'iso': int(h.get('ISOSPEED', 0) or 0), 'utc': t, 'hfr': 0, 'stars': 0, 'verdict': 'listed'})
        if not a.ref: sys.exit('--files needs --ref')
        g = {'name': a.name, 'filter': 'listed', 'aliases': [a.name], 'rows': rows}
        return run(g, a.out or os.path.join(a.data, 'finals'), a.jobs, a.kappa, None, a.ref)      # writes OUT/<name>/
    if not a.night: sys.exit('need --night, --files or --combine')
    out = a.out or os.path.join(a.data, 'finals', 'fullres_' + a.night.replace('-', ''))
    from datetime import datetime, timedelta, timezone
    t0 = datetime.fromisoformat(a.night).replace(hour=12, tzinfo=timezone.utc); t1 = t0 + timedelta(days=1)
    verd = set(a.verdicts.split(','))
    rows = [r for r in restack.frames(a.data) if t0 <= r['utc'] < t1]
    rows = [r for r in rows if r['verdict'] in verd]
    groups = [g for g in restack.group(rows) if len(g['rows']) >= a.min_frames]
    if a.only: groups = [g for g in groups if any(o.lower() in (g['name'] + ' ' + ' '.join(g['aliases'])).lower() for o in a.only.split(','))]
    pf = None
    if not a.no_pattern:
        os.makedirs(out, exist_ok=True); pf = os.path.join(out, 'pattern.fits')
        allr = sorted((r for g in groups for r in g['rows']), key=lambda r: r['utc'])
        pick = allr[::max(1, -(-len(allr) // 700))]          # all frames up to 700 (8.8 GB): a 170-frame pattern carried 59 ADU of its own noise against 124 ADU of real pattern (split-half correlation 0.81, 11 Nov)
        if len({g['name'] for g in groups}) < 2: print('warning: one field only; stars near the rotation centre may print into the pattern', flush=True)
        pat = pattern_from([r['file'] for r in pick], pf)
        print(f"pattern from {len(pick)} frames: rms {float(np.std(pat)):.1f} ADU, pixels over 200 ADU {int((np.abs(pat) > 200).sum())}", flush=True)
    for g in sorted(groups, key=lambda g: -len(g['rows'])): run(g, out, a.jobs, a.kappa, pf)


if __name__ == '__main__':
    main()
