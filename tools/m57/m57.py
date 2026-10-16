#!/usr/bin/env python3
"""M57 from raw Origin frames: select, register, CFA drizzle (2x), measure the PSF from the stack's own stars, Richardson-Lucy, and a split-half check.
Everything is computed from the FITS files named in the frame list; nothing is typed in by hand except the crop centre of the target (pixel position in the frames).

    m57.py frames   --seq SEQ.jsonl --from T0 --to T1 --out frames.txt
    m57.py register --list frames.txt --out reg.pkl
    m57.py drizzle  --list frames.txt --reg reg.pkl --subset all|odd|even --out drz_all.npy
    m57.py deconv   --drz drz_all.npy --iters 28 --out dir/        (writes PSF, tif/png before and after, star-width table)
    m57.py split    --odd drz_odd.npy --even drz_even.npy --iters 28 --out split.json
Layout: Bayer GRBG, crop +-420 px around (1456, 624), output 2x (0.739 arcsec/px), 255 frames of 20 s, ISO 800.
"""
import argparse, json, pickle, sys, time
import numpy as np
from astropy.io import fits
from scipy.ndimage import gaussian_filter, maximum_filter
from scipy.spatial import cKDTree
from multiprocessing import Pool

CX, CY, R, S = 1456, 624, 420, 2
OW = 2 * R * S
CH = np.empty((2 * R, 2 * R), np.int8); CH[0::2, 0::2] = 0; CH[0::2, 1::2] = 1; CH[1::2, 0::2] = 2; CH[1::2, 1::2] = 0     # GRBG: 0 G, 1 R, 2 B
_G = {}

def load(f):
    d = fits.getdata(f).astype(np.float32); return d[CY - R:CY + R, CX - R:CX + R]

def binl(c):
    b = c.reshape(R, 2, R, 2).mean((1, 3)); return gaussian_filter(b - gaussian_filter(b, 12), 1.5)

def stars(c):
    b = c.reshape(R, 2, R, 2).mean((1, 3)); b = b - gaussian_filter(b, 12)
    g = gaussian_filter(b, 1.2); sig = 1.4826 * np.median(abs(g - np.median(g)))
    y, x = np.nonzero((g == maximum_filter(g, 9)) & (g > 8 * sig)); o = np.argsort(-g[y, x])[:80]; y, x = y[o], x[o]
    out = []
    for yy, xx in zip(y, x):
        if 4 < yy < R - 5 and 4 < xx < R - 5:
            w = b[yy - 3:yy + 4, xx - 3:xx + 4].clip(0); t = w.sum()
            if t > 0:
                gy, gx = np.mgrid[-3:4, -3:4]; out.append((2 * (xx + (w * gx).sum() / t) + 0.5, 2 * (yy + (w * gy).sum() / t) + 0.5))
    return np.array(out)

def match(ref, pts, T):
    tr = cKDTree(ref)
    for rad in (6, 3, 1.5, 1.5):
        d, i = tr.query(pts @ T[:, :2].T + T[:, 2], distance_upper_bound=rad); ok = np.isfinite(d)
        if ok.sum() < 6: return T, int(ok.sum()), 9.0
        A, B = pts[ok], ref[i[ok]]; M = np.zeros((2 * len(A), 4)); v = np.zeros(2 * len(A))
        M[0::2] = np.c_[A[:, 0], -A[:, 1], np.ones(len(A)), np.zeros(len(A))]; M[1::2] = np.c_[A[:, 1], A[:, 0], np.zeros(len(A)), np.ones(len(A))]
        v[0::2] = B[:, 0]; v[1::2] = B[:, 1]; p = np.linalg.lstsq(M, v, rcond=None)[0]; T = np.array([[p[0], -p[1], p[2]], [p[1], p[0], p[3]]])
    d, i = tr.query(pts @ T[:, :2].T + T[:, 2], distance_upper_bound=1.5); ok = np.isfinite(d)
    return T, int(ok.sum()), float(np.sqrt((d[ok] ** 2).mean())) if ok.any() else 9.0

def shift0(a, b):
    a = a - a.mean(); b = b - b.mean()
    cc = np.fft.fftshift(np.fft.irfft2(np.fft.rfft2(a) * np.conj(np.fft.rfft2(b)), s=a.shape)); y, x = np.unravel_index(np.argmax(cc), cc.shape)
    return int(x - a.shape[1] // 2), int(y - a.shape[0] // 2)

def cmd_frames(a):
    rows = [json.loads(l) for l in open(a.seq) if l.strip()]
    sel = [r for r in rows if a.t0 <= r['time'] <= a.t1 and r['state'] == 'shooting' and r.get('verdict') == 'clear' and r.get('stacked')]
    open(a.out, 'w').write('\n'.join(r['file'] for r in sel) + '\n')
    json.dump({r['file']: r['hfr_px'] for r in sel}, open(a.out + '.hfr.json', 'w'))
    print(len(sel), 'frames', sel[0]['time'], sel[-1]['time'])

def cmd_register(a):
    from scipy.ndimage import rotate
    files = open(a.list).read().split(); n = len(files); mid = n // 2
    c0 = load(files[mid]); ref = stars(c0); rb = binl(c0); Ts = [None] * n; ok = [False] * n; info = [None] * n
    Ts[mid] = np.array([[1, 0, 0], [0, 1, 0]], float); ok[mid] = True
    for rng in (range(mid + 1, n), range(mid - 1, -1, -1)):          # chain from the neighbour's transform
        prev = Ts[mid]
        for i in rng:
            T, m, rms = match(ref, stars(load(files[i])), prev); info[i] = (m, rms)
            if m >= 15 and rms < 0.8: Ts[i] = T; ok[i] = True; prev = T
    for i in range(n):                                               # rescue: correlation start over rotations
        if ok[i]: continue
        c = load(files[i]); p = stars(c); b = binl(c); best = None
        for ang in np.arange(-4, 2.01, 0.5):
            sx, sy = shift0(rb, rotate(b, ang, reshape=False, order=1))
            for sgn in (1, -1):
                th = np.radians(sgn * ang); Rm = np.array([[np.cos(th), -np.sin(th)], [np.sin(th), np.cos(th)]])
                T, m, rms = match(ref, p, np.c_[Rm, np.array([R, R]) - Rm @ np.array([R, R]) + 2 * np.array([sx, sy])])
                if m >= 15 and rms < 0.8 and (best is None or m > best[1]): best = (T, m, rms)
        if best: Ts[i] = best[0]; ok[i] = True; info[i] = best[1:]
    ang = [np.degrees(np.arctan2(T[1, 0], T[0, 0])) for T in Ts if T is not None]
    print(sum(ok), 'of', n, 'registered; rotation %.2f..%.2f deg; median match rms %.2f px' % (min(ang), max(ang), np.median([x[1] for x, o in zip(info, ok) if o and x])))
    pickle.dump((Ts, ok, info), open(a.out, 'wb'))

def deposit(c, T, mean, wt, clipk, sig):
    sm = np.zeros((3, OW, OW)); sw = np.zeros((3, OW, OW)); ys, xs = np.mgrid[0:2 * R, 0:2 * R]
    u = S * (T[0, 0] * xs + T[0, 1] * ys + T[0, 2] + 0.5) - 0.5; v = S * (T[1, 0] * xs + T[1, 1] * ys + T[1, 2] + 0.5) - 0.5
    u0 = np.floor(u).astype(int); v0 = np.floor(v).astype(int); fu = u - u0; fv = v - v0
    inb = (u0 >= 0) & (u0 < OW - 1) & (v0 >= 0) & (v0 < OW - 1); keep = np.ones(c.shape, bool)
    if mean is not None:                                             # reject samples far from the first-pass mean of the same colour
        m = np.zeros(c.shape, np.float32)
        for k in range(3):
            s = (CH == k) & inb; A = mean[k]; i = v0[s]; j = u0[s]; p = fu[s]; q = fv[s]
            m[s] = A[i, j] * (1 - p) * (1 - q) + A[i, j + 1] * p * (1 - q) + A[i + 1, j] * (1 - p) * q + A[i + 1, j + 1] * p * q
        keep = abs(c - m) < clipk * sig + 0.12 * abs(m)
    for k in range(3):
        s = (CH == k) & keep & inb; val = c[s]; i = v0[s]; j = u0[s]; p = fu[s]; q = fv[s]
        for di, dj, w in ((0, 0, (1 - p) * (1 - q)), (0, 1, p * (1 - q)), (1, 0, (1 - p) * q), (1, 1, p * q)):
            idx = (i + di) * OW + (j + dj)
            sm[k].ravel()[:] += np.bincount(idx, weights=val * w * wt, minlength=OW * OW); sw[k].ravel()[:] += np.bincount(idx, weights=w * wt, minlength=OW * OW)
    return sm, sw

def _work(i):
    f = _G['files'][i]; c = load(f).astype(np.float64); sig = []
    for k in range(3):
        v = c[CH == k]; bg = np.median(v); c[CH == k] -= bg; sig.append(1.4826 * np.median(abs(v - bg)))
    s = float(np.mean(sig)); w = (2.3 / _G['hfr'][f]) ** 2 / s ** 2                 # weight: 1/noise^2 and sharpness
    return deposit(c.astype(np.float32), _G['Ts'][i], _G['mean'], w, 4.0, s)

def _acc(n):
    S_ = np.zeros((3, OW, OW)); W_ = np.zeros((3, OW, OW))
    with Pool(11) as p:
        for sm, sw in p.imap_unordered(_work, n): S_ += sm; W_ += sw
    return S_ / np.maximum(W_, 1e-12), W_

def cmd_drizzle(a):
    files = open(a.list).read().split(); Ts, ok, _ = pickle.load(open(a.reg, 'rb'))
    _G.update(files=files, Ts=Ts, hfr=json.load(open(a.list + '.hfr.json')), mean=None)
    idx = [i for i in range(len(files)) if ok[i] and (a.subset == 'all' or i % 2 == (a.subset == 'odd'))]
    t = time.time(); m1, _ = _acc(idx); _G['mean'] = m1.astype(np.float32); m2, w2 = _acc(idx)
    np.save(a.out, m2.astype(np.float32)); print(len(idx), 'frames drizzled in %.0f s' % (time.time() - t))

def rgbl(drz):
    rgb = np.stack([drz[1], drz[0], drz[2]]).astype(np.float64)
    for k in range(3): rgb[k] -= np.median(rgb[k])
    L = rgb.sum(0) / 3; return rgb, L, 1.4826 * np.median(abs(L - np.median(L)))

def measure_psf(L, sig):
    from scipy.optimize import curve_fit
    from scipy.ndimage import shift as nshift
    g = gaussian_filter(L, 1.5); pk = (g == maximum_filter(g, 25)) & (g > 40 * sig) & (g < 0.3 * L.max()); ys, xs = np.nonzero(pk)
    k = (ys > 40) & (ys < OW - 40) & (xs > 40) & (xs < OW - 40); ys, xs = ys[k], xs[k]; H = 15; stk = []; fw = []; yy, xx = np.mgrid[-H:H + 1, -H:H + 1]
    for y, x in zip(ys, xs):
        c = L[y - H:y + H + 1, x - H:x + H + 1]
        if c.max() > 0.8 * np.percentile(L, 99.99): continue
        try: p, _ = curve_fit(lambda q, A, x0, y0, s, b: (A * np.exp(-((q[0] - x0) ** 2 + (q[1] - y0) ** 2) / (2 * s * s)) + b).ravel(), (xx, yy), c.ravel(), p0=[c.max(), 0, 0, 2.2, 0])
        except Exception: continue
        if abs(p[1]) > 1 or abs(p[2]) > 1 or not 1.5 < p[3] < 4 or (c[np.hypot(xx - p[1], yy - p[2]) > 9] > 6 * sig).any(): continue
        st = nshift(c - p[4], (-p[2], -p[1]), order=3); stk.append(st / st.sum()); fw.append(2.355 * p[3])
    psf = np.median(stk, 0).clip(0); return psf / psf.sum(), len(stk), float(np.median(fw)), (ys, xs)

def star_fwhm(img, ys, xs):
    out = []
    for y, x in zip(ys, xs):
        if img[y, x] > 0: c = img[y - 6:y + 7, x - 6:x + 7]; out.append(2 * np.sqrt((c > c.max() / 2).sum() / np.pi))
    return float(np.median(out))

def rl(L, psf, iters):
    from skimage.restoration import richardson_lucy
    mx = L.max(); return (richardson_lucy(np.clip(L, 0, None) / mx + 1e-4, psf, num_iter=iters, clip=False) - 1e-4) * mx

def cmd_deconv(a):
    import os, tifffile
    from skimage import io
    from scipy.ndimage import zoom
    os.makedirs(a.out, exist_ok=True); drz = np.load(a.drz); rgb, L, sig = rgbl(drz); psf, n, fw, (ys, xs) = measure_psf(L, sig)
    np.save(a.out + '/psf.npy', psf); res = {'psf_stars': n, 'psf_fwhm_out_px': fw, 'psf_fwhm_arcsec': fw * 0.739, 'star_fwhm_px': {'drizzle': star_fwhm(L, ys, xs)}}
    for it in sorted({10, 20, 40, a.iters}): res['star_fwhm_px'][f'RL{it}'] = star_fwhm(rl(L, psf, it), ys, xs)
    d = rl(L, psf, a.iters); Ls = gaussian_filter(L, 1.2)
    rat = np.stack([gaussian_filter(rgb[k], 1.2) for k in range(3)]) / np.maximum(Ls, 1e-3 * sig + 1e-9); rat = np.where(Ls > 3 * sig, rat, 1.0)
    out = np.nan_to_num(np.clip(d, 0, None)[None] * rat); top = np.percentile(L[700:980, 700:980], 99.7)
    st = lambda x: np.clip(np.arcsinh(10 * np.clip(x, 0, None) / top) / np.arcsinh(10), 0, 1)
    for name, im in (('M57_drizzle_only', rgb), (f'M57_drizzle_RL{a.iters}', out)):
        c = zoom(np.stack([st(im[k][690:990, 690:990]) for k in range(3)], -1), (3, 3, 1), order=3).clip(0, 1)
        tifffile.imwrite(f'{a.out}/{name}.tif', (c * 65535).astype(np.uint16)); io.imsave(f'{a.out}/{name}.png', (c * 255).astype(np.uint8))
    np.save(a.out + f'/RL{a.iters}_L.npy', d.astype(np.float32)); json.dump(res, open(a.out + '/deconv.json', 'w'), indent=1); print(json.dumps(res))

def cmd_split(a):
    """Two stacks from disjoint frames (odd / even), each deconvolved with its own measured PSF. Detail gained by deconvolution (RL minus drizzle) that is real appears in both;
    amplified noise does not. Reported: Pearson correlation of that gained-detail image between the halves, inside the nebula and on blank sky, and the same for the plain drizzles."""
    out = {}; gain = []; plain = []
    for f in (a.odd, a.even):
        rgb, L, sig = rgbl(np.load(f)); psf, *_ = measure_psf(L, sig); d = rl(L, psf, a.iters)
        gain.append((d - L, L)); plain.append(L)
    yy, xx = np.mgrid[0:OW, 0:OW]; r = np.hypot(xx - 840, yy - 840)
    Lm = (plain[0] + plain[1]) / 2; sg = 1.4826 * np.median(abs(Lm - np.median(Lm)))
    from scipy.ndimage import binary_dilation
    star = binary_dilation(gaussian_filter(Lm, 1.5) > 5 * sg, iterations=14)               # every star and its wings, the nebula itself excluded below
    ring = (r < 90)                                                                          # the nebula (its own pixels, with the central star)
    star &= ~ring; star |= (np.hypot(xx - 840, yy - 840) < 6)                               # central star excluded from the nebula test (it is a point source)
    masks = {'nebula_excl_central_star': ring & ~(r < 6), 'blank_sky_stars_masked': (r > 200) & (r < 650) & ~star}
    for name, m in masks.items():
        c = lambda A, B: float(np.corrcoef(A[m], B[m])[0, 1])
        hp = [g[0] - gaussian_filter(g[0], 4) for g in gain]                                 # fine-scale part of the gain only
        out[name] = {'pixels': int(m.sum()), 'gain_corr_odd_vs_even': c(gain[0][0], gain[1][0]), 'fine_gain_corr_odd_vs_even': c(hp[0], hp[1]), 'drizzle_corr_odd_vs_even': c(plain[0], plain[1]),
                     'gain_rms_over_sky_noise': float(np.std(gain[0][0][m]) / sg)}
    json.dump(out, open(a.out, 'w'), indent=1); print(json.dumps(out, indent=1))

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter); sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('frames'); p.add_argument('--seq', required=True); p.add_argument('--from', dest='t0', required=True); p.add_argument('--to', dest='t1', required=True); p.add_argument('--out', required=True)
    p = sp.add_parser('register'); p.add_argument('--list', required=True); p.add_argument('--out', required=True)
    p = sp.add_parser('drizzle'); p.add_argument('--list', required=True); p.add_argument('--reg', required=True); p.add_argument('--subset', default='all', choices=['all', 'odd', 'even']); p.add_argument('--out', required=True)
    p = sp.add_parser('deconv'); p.add_argument('--drz', required=True); p.add_argument('--iters', type=int, default=28); p.add_argument('--out', required=True)
    p = sp.add_parser('split'); p.add_argument('--odd', required=True); p.add_argument('--even', required=True); p.add_argument('--iters', type=int, default=28); p.add_argument('--out', required=True)
    a = ap.parse_args(); globals()['cmd_' + a.cmd](a)
