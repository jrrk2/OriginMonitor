#!/usr/bin/env python3
"""Does luminance/chrominance recover resolution in the Saturn colour stack?

Colour comes from the per-plane interpolated stack (soft); luminance comes from the drizzle that uses every Bayer sample at its
true position (sharp). Output = colour * (Y_hi / lowpass(Y_hi)), i.e. the colour is kept, the high spatial frequencies come from Y_hi.
Compared by the vertical FWHM of the ring ansae (arcsec), measured identically on every image.
"""
import argparse, os, sys
import cv2, numpy as np
from scipy import ndimage as ndi
from skimage.registration import phase_cross_correlation
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import planet_triptych as T
import planets as P
import saturn_moons as SM

ARC = P.ARC

def parts(work):
    Z = np.load(os.path.join(work, 'crops.npz')); poly = np.load(os.path.join(work, 'calib_poly.npy')); q, t = Z['q'], Z['t']
    idx = np.argsort(-q[:500])[:40]
    # --- soft colour stack (what the triptych used)
    imgs = []; ref = None
    for i in idx:
        rgb = T.interp_rgb(Z['crops'][i].astype(np.float32) - P.BL); n = rgb.shape[0]
        rgb = np.dstack([SM.northup(rgb[..., k].astype(np.float32), Z['orient'][i] + SM.corr_angle(t[i], poly), Z['cx'][i], Z['cy'][i], n) for k in range(3)])
        if ref is None: ref = rgb[..., 1]
        sh = phase_cross_correlation(ref[40:-40, 40:-40], rgb[40:-40, 40:-40, 1], upsample_factor=30, normalization=None)[0]
        imgs.append(np.dstack([ndi.shift(rgb[..., k], np.clip(sh, -3, 3), order=3) for k in range(3)]))
    rgb_lo = np.mean(imgs, 0)
    # --- sharp luminance: register on half-res luminance, drizzle all Bayer samples with the per-frame derotation
    SM.G.update(crops=Z['crops'], cx=Z['cx'], cy=Z['cy'], ori=Z['orient'], t=t, q=q, poly=poly)
    NB = SM.NB; r0 = slice(NB // 2 - 24, NB // 2 + 24); win = np.hanning(48)[:, None] * np.hanning(48)[None, :]
    L = np.array([SM.northup(SM.bin2(Z['crops'][i].astype(np.float32) - SM.BL), Z['orient'][i] + SM.corr_angle(t[i], poly), Z['cx'][i] / 2, Z['cy'][i] / 2, NB) for i in idx])
    def pcc(a, b): return np.clip(phase_cross_correlation(a[r0, r0] * win, b[r0, r0] * win, upsample_factor=20, normalization=None)[0], -3, 3)
    sh = [np.zeros(2)] + [pcc(L[0], L[k]) for k in range(1, len(L))]
    for _ in range(2):
        refm = np.array([ndi.shift(L[k], sh[k], order=3) for k in range(len(L))]).mean(0); sh = [pcc(refm, L[k]) for k in range(len(L))]
    y_hi = SM.drizzle(idx, sh)                                        # 2x grid, planet at the centre
    return rgb_lo, y_hi

def lrgb(rgb_lo, y_hi, sigma=2.4):
    n = y_hi.shape[0]; c = rgb_lo.shape[0] // 2; h = n // 4                               # y_hi is 2x grid: +-60 raw px -> n=240
    lo = rgb_lo[c - 60:c + 60, c - 60:c + 60]; up = cv2.resize(lo.astype(np.float32), (n, n), interpolation=cv2.INTER_CUBIC)
    y_lo = 0.25 * up[..., 0] + 0.5 * up[..., 1] + 0.25 * up[..., 2]
    sh = phase_cross_correlation(y_hi / y_hi.max(), y_lo / y_lo.max(), upsample_factor=30, normalization=None)[0]           # residual grid offset
    up = np.dstack([ndi.shift(up[..., k], sh, order=3) for k in range(3)])
    yh = np.clip(y_hi, 0, None); ratio = yh / (cv2.GaussianBlur(yh, (0, 0), sigma) + 0.03 * yh.max())
    return np.clip(up * ratio[..., None], 0, None), up, sh

def ansa_fwhm(img, px):
    """vertical FWHM (arcsec) of the ring ansae, 14-21 arcsec from the planet, after levelling the ring axis."""
    g = np.clip(img, 0, None).astype(np.float32); cy, cx = ndi.center_of_mass(g * (g > g.max() * 0.4))
    ang = SM.axis_angle(g, 0.08); M = cv2.getRotationMatrix2D((cx, cy), ang, 1.0); g = cv2.warpAffine(g, M, g.shape[::-1], flags=cv2.INTER_CUBIC)
    out = []
    for side in (-1, 1):
        x0 = int(round(cx + side * 14 / px)); x1 = int(round(cx + side * 21 / px)); xa, xb = sorted((x0, x1))
        prof = g[:, xa:xb].mean(1); y = np.arange(len(prof)); k = int(np.argmax(ndi.gaussian_filter1d(prof[int(cy - 12 / px):int(cy + 12 / px)], 1))) + int(cy - 12 / px)
        base = np.median(prof[int(cy - 30 / px):int(cy - 20 / px)]); pk = prof[k - 1:k + 2].max() - base; half = base + pk / 2
        lo = k
        while lo > 0 and prof[lo] > half: lo -= 1
        hi = k
        while hi < len(prof) - 1 and prof[hi] > half: hi += 1
        l = lo + (half - prof[lo]) / (prof[lo + 1] - prof[lo] + 1e-9); r = hi - 1 + (prof[hi - 1] - half) / (prof[hi - 1] - prof[hi] + 1e-9)
        out.append(((r - l) * px, pk / (g[int(cy), int(cx)] + 1e-9)))
    return np.mean([o[0] for o in out]), np.mean([o[1] for o in out])

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--work', default='build/saturn_moons'); ap.add_argument('--out', default=os.path.expanduser('~/Pictures/CelestronStacks/planets')); a = ap.parse_args()
    rgb_lo, y_hi = parts(a.work); res = {}
    old = cv2.imread(os.path.expanduser('~/Pictures/CelestronStacks/saturn_best5pct_21f_16bit.png'), cv2.IMREAD_UNCHANGED).astype(np.float32)
    g_lo = rgb_lo[..., 1]; y_lo = 0.25 * rgb_lo[..., 0] + 0.5 * rgb_lo[..., 1] + 0.25 * rgb_lo[..., 2]
    out, up, sh = lrgb(rgb_lo, y_hi); y_out = 0.25 * out[..., 0] + 0.5 * out[..., 1] + 0.25 * out[..., 2]
    print('residual grid offset between drizzle luminance and colour stack: (%.2f, %.2f) grid px' % tuple(sh))
    rows = [('earlier best-5% mono still (drizzle, 21 frames)', old, ARC / 2), ('triptych colour stack, luminance (soft interpolation)', y_lo, ARC),
            ('new drizzle luminance, 40 frames, derotated', y_hi, ARC / 2), ('LRGB result, luminance', y_out, ARC / 2)]
    print('\nring-ansa vertical FWHM (smaller = sharper), and ansa peak / disc brightness:')
    for name, im, px in rows:
        try: f, r = ansa_fwhm(im, px); print('  %-58s FWHM %.1f"   ansa/disc %.3f' % (name, f, r))
        except Exception as e: print('  %-58s (measurement failed: %s)' % (name, e))
    # save visual comparison
    def to8(x, hi, gam=0.7): return (np.clip(x / hi, 0, 1) ** gam * 255).astype(np.uint8)
    c = 120; sl = slice(c - 40, c + 40)
    tiles = []
    for name, im, px in rows[:1]:
        cc = im[100 - 40:100 + 40, 100 - 40:100 + 40]; tiles.append(cv2.cvtColor(cv2.resize(to8(cc, np.percentile(cc, 99.9)), (400, 400), interpolation=cv2.INTER_CUBIC), cv2.COLOR_GRAY2BGR))
    upm = cv2.resize(rgb_lo[150 - 20:150 + 20, 150 - 20:150 + 20].astype(np.float32), (400, 400), interpolation=cv2.INTER_CUBIC); hi = np.percentile(upm[..., 1], 99.9)
    tiles.append((np.clip(upm / hi, 0, 1) ** 0.8 * 255).astype(np.uint8)[..., ::-1])
    tiles.append(cv2.cvtColor(cv2.resize(to8(y_hi[sl, sl], np.percentile(y_hi[sl, sl], 99.9)), (400, 400), interpolation=cv2.INTER_CUBIC), cv2.COLOR_GRAY2BGR))
    o = out[sl, sl]; tiles.append((np.clip(cv2.resize(o.astype(np.float32), (400, 400), interpolation=cv2.INTER_CUBIC) / np.percentile(o[..., 1], 99.9), 0, 1) ** 0.8 * 255).astype(np.uint8)[..., ::-1])
    labels = ['earlier best 5% (mono)', 'triptych colour stack', 'new drizzle luminance', 'LRGB (colour + sharp luminance)']
    tiles = [np.ascontiguousarray(t_) for t_ in tiles]
    for t_, l in zip(tiles, labels): cv2.putText(t_, l, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 0), 1, cv2.LINE_AA)
    cv2.imwrite(os.path.join(a.out, 'saturn_lrgb_comparison.png'), np.hstack(tiles)); cv2.imwrite(os.path.join(a.out, 'saturn_lrgb.png'), (np.clip(cv2.resize(out[sl, sl].astype(np.float32), (480, 480), interpolation=cv2.INTER_CUBIC) / np.percentile(out[sl, sl][..., 1], 99.9), 0, 1) ** 0.8 * 255).astype(np.uint8)[..., ::-1])
    np.save('build/saturn_lrgb.npy', out)

if __name__ == '__main__':
    main()
