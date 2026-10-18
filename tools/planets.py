#!/usr/bin/env python3
"""Lucky-imaging analysis of short planetary bursts (raw 16-bit Origin FITS): Mars and Jupiter.

    tools/planets.py --mars 'build/planets/snapshot_*.fits build/planets/mars/*.fits' --jupiter 'build/planets/jupiter/*.fits' \
                     --out ~/Pictures/CelestronStacks/planets

Per target: crop around the planet, rate frames (Fourier high-frequency fraction), register by FFT phase correlation,
drizzle the gain-equalised Bayer planes 3x, stack best-N, report measured size/sharpness. Jupiter: identify the Galilean
moons against JPL Horizons (calibrating the image rotation from the moons themselves) and measure the belts.
"""
import argparse, glob, json, os, sys
from datetime import datetime, timedelta
import cv2, numpy as np
from astropy.io import fits
from scipy import ndimage as ndi
from skimage.registration import phase_cross_correlation

BL = 4985.0
# Bayer layout (verified on field stars and planets): greens on the diagonal (0,0),(1,1); red at (0,1); blue at (1,0) (GRBG, not the header's RGGB).
# GAIN is a provisional white balance: median field star set neutral (R/G and B/G of 387 stars were 0.65 and 0.67).
GAIN = np.array([[1.0, 1 / 0.65], [1 / 0.67, 1.0]], np.float32)
ARC = 0.000410465 * 3600
CR = 128                                                          # raw crop
HALF = 40; S = 3                                                  # drizzle half-width (raw px) and scale


def bin2(d): return d.reshape(d.shape[0] // 2, 2, d.shape[1] // 2, 2).sum((1, 3))
def equalise(d):
    e = d.copy()
    for i in (0, 1):
        for j in (0, 1): e[i::2, j::2] *= GAIN[i, j]
    return e
def centroid(b):
    sm = ndi.gaussian_filter(b, 3); y, x = np.unravel_index(sm.argmax(), sm.shape)
    y0, x0 = max(y - 40, 0), max(x - 40, 0); m = b[y0:y + 41, x0:x + 41]; m = np.clip(m - m.max() * 0.25, 0, None)
    cy, cx = ndi.center_of_mass(m); return y0 + cy, x0 + cx
def hfq(e, cy, cx):
    g = ndi.uniform_filter(e, 2); y = int(round(cy)); x = int(round(cx)); g = g[y - 48:y + 48, x - 48:x + 48]
    if g.shape != (96, 96): return 0.0
    g = g / g.max(); w = np.hanning(96)[:, None] * np.hanning(96)[None, :]
    F = np.abs(np.fft.fftshift(np.fft.fft2((g - g.mean()) * w))) ** 2; r = np.hypot(*np.mgrid[-48:48, -48:48])
    return float(F[(r > 6) & (r < 30)].sum() / F[r < 6].sum())

def load(paths, exposure):
    fr = []
    for f in paths:
        h = fits.getheader(f)
        if abs(h['EXPTIME'] - exposure) > 1e-9: continue
        d = fits.getdata(f); b = bin2(d.astype(np.float32) - BL); cy, cx = centroid(b); cy *= 2; cx *= 2
        y0 = int(round(cy)) - CR // 2; x0 = int(round(cx)) - CR // 2; y0 -= y0 % 2; x0 -= x0 % 2
        y0 = min(max(y0, 0), d.shape[0] - CR); x0 = min(max(x0, 0), d.shape[1] - CR)
        crop = d[y0:y0 + CR, x0:x0 + CR].astype(np.float32) - BL
        fr.append(dict(f=f, crop=crop, cy=cy - y0, cx=cx - x0, q=hfq(equalise(crop), cy - y0, cx - x0), peak=float(d.max()), clip=int((d >= 65535).sum()),
                       t=datetime.strptime(h['DATE-OBS'], '%Y-%m-%dT%H:%M:%S') - timedelta(hours=1), orient=h['ORIENTAT'], full=(y0, x0)))
    return fr

def drizzle(frames, shifts, planes=((0, 0), (0, 1), (1, 0), (1, 1)), gains=None):
    n = CR // 2; size = 2 * HALF * S; acc = np.zeros(size * size); wt = np.zeros(size * size); jj, ii = np.meshgrid(np.arange(n), np.arange(n))
    for fr, sh in zip(frames, shifts):
        eq = equalise(fr['crop']) if gains is None else fr['crop'] * np.tile(np.asarray(gains, np.float32), (CR // 2, CR // 2))
        for oy, ox in planes:
            v = eq[oy::2, ox::2].ravel(); dx = (2 * jj + ox - fr['cx']).ravel() + sh[1]; dy = (2 * ii + oy - fr['cy']).ravel() + sh[0]
            u = (dx + HALF) * S; w = (dy + HALF) * S; u0 = np.floor(u).astype(int); w0 = np.floor(w).astype(int); fu = u - u0; fw = w - w0
            for du, dw, wgt in ((0, 0, (1 - fu) * (1 - fw)), (1, 0, fu * (1 - fw)), (0, 1, (1 - fu) * fw), (1, 1, fu * fw)):
                uu = u0 + du; ww = w0 + dw; ok = (uu >= 0) & (uu < size) & (ww >= 0) & (ww < size); k = ww[ok] * size + uu[ok]
                acc += np.bincount(k, weights=(v * wgt)[ok], minlength=size * size); wt += np.bincount(k, weights=wgt[ok], minlength=size * size)
    return (acc / np.maximum(wt, 1e-6)).reshape(size, size)

def register(frames):
    order = np.argsort([-f['q'] for f in frames]); ref = ndi.uniform_filter(equalise(frames[order[0]]['crop']), 2)
    sh = {}
    for it in range(2):
        for i, f in enumerate(frames):
            im = ndi.uniform_filter(equalise(f['crop']), 2)
            # centroid-aligned crops differ only by sub-pixel residuals
            shift = phase_cross_correlation(ref, im, upsample_factor=30, normalization=None)[0]
            sh[i] = np.clip(shift - np.array([f['cy'] - frames[order[0]]['cy'], f['cx'] - frames[order[0]]['cx']]) * 0, -4, 4)
        ref = np.mean([ndi.shift(ndi.uniform_filter(equalise(f['crop']), 2), sh[i], order=3) for i, f in enumerate(frames)], 0)
    return [sh[i] for i in range(len(frames))]

def gaussian_fwhm(img):
    """Moment-based FWHM (arcsec of raw pixel scale) of a compact source from background-subtracted pixels above 5% of peak."""
    img = np.clip(img - np.median(img[:10]), 0, None); y, x = np.mgrid[:img.shape[0], :img.shape[1]]
    m = img > img.max() * 0.05; w = img * m; t = w.sum(); cx = (x * w).sum() / t; cy = (y * w).sum() / t
    var = ((x - cx) ** 2 * w).sum() / t / 2 + ((y - cy) ** 2 * w).sum() / t / 2
    return 2.355 * np.sqrt(var)

def stackset(frames, label, out):
    """drizzled stacks of all / best 50% / best 20% frames; returns images and the shifts"""
    sh = register(frames); order = np.argsort([-f['q'] for f in frames]); res = {}
    for name, frac in (('all', 1.0), ('best50', 0.5), ('best20', 0.2)):
        k = max(3, int(round(len(frames) * frac))); idx = order[:k]; res[name] = (drizzle([frames[i] for i in idx], [sh[i] for i in idx]), k)
    return res, order, sh

def to16(a, lo=0, hi=None):
    hi = hi or np.percentile(a, 99.99); return (np.clip((a - lo) / (hi - lo), 0, 1) * 65535).astype(np.uint16)
def to8(a, hi, g=0.7): return (np.clip(a / hi, 0, 1) ** g * 255).astype(np.uint8)

def plane_fluxes(fr, rad=30):
    c = fr['crop']; y, x = np.mgrid[:CR, :CR]; m = np.hypot(y - fr['cy'], x - fr['cx']) < rad; out = []
    for oy, ox in ((0, 0), (0, 1), (1, 0), (1, 1)):
        p = c[oy::2, ox::2]; mm = m[oy::2, ox::2]; out.append(float(p[mm].sum()))
    return out

def interp(eph, name, t):
    rows = eph[name]; ts = [datetime.strptime(r[0], '%Y-%b-%d %H:%M') for r in rows]
    k = max(0, min(len(ts) - 2, int((t - ts[0]).total_seconds() // 300))); w = (t - ts[k]).total_seconds() / 300
    return rows[k][1] * (1 - w) + rows[k + 1][1] * w, rows[k][2] * (1 - w) + rows[k + 1][2] * w, float(rows[k][3])

SHIFTS = {}
def colour(frames, order, shifts, frac=0.5):
    """Colour stack at raw resolution: interpolate each colour plane to full resolution (normalised convolution; the sparse
    red/blue planes are too thinly sampled for a 15-frame drizzle), register with the phase-correlation shifts, average."""
    k = max(3, int(round(len(frames) * frac))); idx = order[:k]
    planes = {'R': ((0, 1),), 'G': ((0, 0), (1, 1)), 'B': ((1, 0),)}; sig = {'R': 1.1, 'G': 0.8, 'B': 1.1}
    acc = np.zeros((CR, CR, 3))
    for i in idx:
        fr = frames[i]; eq = equalise(fr['crop']); chans = []
        for c in 'RGB':
            m = np.zeros((CR, CR))
            for oy, ox in planes[c]: m[oy::2, ox::2] = 1
            chans.append(ndi.gaussian_filter(eq * m, sig[c]) / np.maximum(ndi.gaussian_filter(m, sig[c]), 1e-6))
        for j, ch in enumerate(chans):
            acc[..., j] += ndi.shift(ch, (CR / 2 - fr['cy'] + shifts[i][0], CR / 2 - fr['cx'] + shifts[i][1]), order=3)
    return acc / len(idx)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--mars', required=True); ap.add_argument('--jupiter', required=True)
    ap.add_argument('--eph', default='build/planets/eph.json'); ap.add_argument('--out', default=os.path.expanduser('~/Pictures/CelestronStacks/planets')); a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True); eph = json.load(open(a.eph)); report = {}
    sets = {}
    for name, pat, exp in (('mars', a.mars, 0.001), ('jupiter', a.jupiter, 0.0008)):
        paths = sorted(sum([glob.glob(os.path.expanduser(p)) for p in pat.split()], []))
        fr = load(paths, exp); res, order, shf = stackset(fr, name, a.out); sets[name] = (fr, res, order); SHIFTS[name] = shf
        q = np.array([f['q'] for f in fr]); print('\n== %s: %d frames at %.1f ms | sharpness metric min/median/best %.3f/%.3f/%.3f | clipped frames %d | peak median %d'
              % (name.upper(), len(fr), exp * 1e3, q.min(), np.median(q), q.max(), sum(1 for f in fr if f['clip']), np.median([f['peak'] for f in fr])))
        best = res['best20'][0]; hi = np.percentile(best, 99.99)
        for k, (img, n) in res.items(): cv2.imwrite(os.path.join(a.out, f'{name}_{k}_{n}f_16bit.png'), to16(np.clip(img, 0, None), 0, hi))
        # metrics
        fw = {k: gaussian_fwhm(img) / S * ARC for k, (img, n) in res.items()}; report[name] = dict(frames=len(fr), fwhm_arcsec=fw)
        print('   measured blob FWHM (arcsec): ' + ', '.join('%s %.2f' % (k, v) for k, v in fw.items()))
        sets[name] += (hi,)
    # ---- Mars: apparent size vs seeing
    fr, res, order, hi = sets['mars']; print('\nMars: predicted angular diameter ~5.8"; raw pixel scale %.2f"/px -> disc spans ~%.1f raw px' % (ARC, 5.8 / ARC))
    # ---- colour check: plane flux ratios (R,G1,G2,B) across the three planets we have
    print('\nBayer plane fluxes inside 30 px of the planet (sky-subtracted), normalised to plane (0,0):')
    for name in ('mars', 'jupiter'):
        fl = np.median([plane_fluxes(f) for f in sets[name][0]], 0); print('   %-8s (0,0)=1  (0,1)=%.2f  (1,0)=%.2f  (1,1)=%.2f' % (name, fl[1] / fl[0], fl[2] / fl[0], fl[3] / fl[0]))
    print('   (Saturn, earlier: (0,1)=0.79  (1,0)=0.46  (1,1)=1.01)')
    # ---- Jupiter's moons: wide stack of full frames, then match to Horizons while solving for the image rotation
    fr = sets['jupiter'][0]; best = [fr[i] for i in sets['jupiter'][2][:len(fr)]]
    acc = []; 
    for f in best:
        d = fits.getdata(f['f']).astype(np.float32) - BL; b = bin2(d); cy, cx = centroid(b); W = 360   # half-res px around Jupiter (+-530")
        y0 = int(round(cy)) - W; x0 = int(round(cx)) - W; ok = y0 >= 0 and x0 >= 0 and y0 + 2 * W <= b.shape[0] and x0 + 2 * W <= b.shape[1]
        if not ok: continue
        acc.append(b[y0:y0 + 2 * W, x0:x0 + 2 * W] if True else None)
        acc[-1] = acc[-1]
    n = len(acc); A = np.array(acc); stack = A.mean(0); hp = stack - ndi.median_filter(stack, size=15); mf = cv2.GaussianBlur(hp, (0, 0), 1.4)
    sg = 1.4826 * np.median(np.abs(mf - np.median(mf))); yy, xx = np.mgrid[:2 * W, :2 * W]; rr = np.hypot(yy - W, xx - W)
    pk = [(v, y, x) for y, x, v in [(p[0], p[1], mf[p[0], p[1]]) for p in np.argwhere((mf == ndi.maximum_filter(mf, 9)) & (mf > 8 * sg))] if rr[v, y] > 8] if False else []
    cand = np.argwhere((mf == ndi.maximum_filter(mf, 9)) & (mf > 8 * sg) & (rr > 8)); cand = sorted(cand, key=lambda p: -mf[p[0], p[1]])[:25]
    print('\nJupiter wide stack of %d frames: noise %.1f ADU; compact sources >8 sigma within +-530": %d' % (n, sg, len(cand)))
    t = fr[len(fr) // 2]['t']; orient = fr[len(fr) // 2]['orient']; jx, jy, _ = interp(eph, 'Jupiter', t)
    pred = {}
    for nm in ('Io', 'Europa', 'Ganymede', 'Callisto'):
        mx, my, mag = interp(eph, nm, t); pred[nm] = (((mx - jx) * np.cos(np.deg2rad(jy))) * 3600, (my - jy) * 3600, mag)
    print('   predicted offsets from Jupiter at %s UTC (E, N arcsec, mag): ' % t.strftime('%H:%M:%S') + '; '.join('%s (%+.0f, %+.0f) %.1f' % (k, *v) for k, v in pred.items()))
    SHh = 2 * ARC
    def to_en(dx, dy, ang):                       # half-res image offset -> (E, N) arcsec using the Saturn-pipeline convention
        r = np.deg2rad(orient + ang); R = np.array([[np.cos(r), np.sin(r)], [np.sin(r), -np.cos(r)]]); v = R @ np.array([dx, dy]) * SHh
        return -v[0], -v[1]
    best_ang = None; best_score = (-1, 0)
    for ang in np.arange(-180, 180, 0.25):
        hits = 0; err = 0
        for nm, (pe, pn, mag) in pred.items():
            if np.hypot(pe, pn) > 530: continue
            dm = min((np.hypot(to_en(p[1] - W, p[0] - W, ang)[0] - pe, to_en(p[1] - W, p[0] - W, ang)[1] - pn) for p in cand), default=1e9)
            if dm < 6: hits += 1; err += dm
        if (hits, -err) > best_score: best_score = (hits, -err); best_ang = ang
    print('   best rotation offset to ORIENTAT: %.2f deg -> %d of 4 Galilean moons matched within 6" (mean error %.1f")' % (best_ang, best_score[0], -best_score[1] / max(best_score[0], 1)))
    for nm, (pe, pn, mag) in pred.items():
        c = [(np.hypot(to_en(p[1] - W, p[0] - W, best_ang)[0] - pe, to_en(p[1] - W, p[0] - W, best_ang)[1] - pn), p) for p in cand]
        d, p = min(c, key=lambda z: z[0]); print('      %-9s predicted (%+5.0f,%+5.0f)"  %s' % (nm, pe, pn, ('found %.1f" away, peak %.0f ADU = %.0f sigma' % (d, mf[p[0], p[1]], mf[p[0], p[1]] / sg)) if d < 6 else 'not found (nearest %.0f" away)' % d))
    # ---- equator axis from the matched moons (they orbit in Jupiter's equatorial plane)
    found = []
    for nm, (pe, pn, mag) in pred.items():
        c = [(np.hypot(to_en(p[1] - W, p[0] - W, best_ang)[0] - pe, to_en(p[1] - W, p[0] - W, best_ang)[1] - pn), p) for p in cand]
        d, p = min(c, key=lambda z: z[0])
        if d < 6: found.append((nm, p))
    vec = np.array([[p[1] - W, p[0] - W] for nm, p in found], float); wts = np.hypot(vec[:, 0], vec[:, 1])
    ang2 = np.angle(np.sum(wts * np.exp(2j * np.arctan2(vec[:, 1], vec[:, 0]))))  # doubled-angle mean (axis, not direction)
    eq = np.degrees(ang2 / 2); print('   equator axis from the moons: %.1f deg from image +x (y down)' % eq)

    # ---- point-source PSF from the moons: unresolved, so their width is the optics + seeing + registration jitter
    fw = []
    for nm, p in found:
        stackm = []
        for f in best:
            d = fits.getdata(f['f']).astype(np.float32) - BL; b2 = bin2(d); cy2, cx2 = centroid(b2)
            X = int(round(cx2 * 2 + 2 * (p[1] - W))); Y = int(round(cy2 * 2 + 2 * (p[0] - W)))
            if not (12 < X < d.shape[1] - 12 and 12 < Y < d.shape[0] - 12): continue
            cut = ndi.uniform_filter(equalise(d[Y - 11:Y + 12, X - 11:X + 12] * 1.0), 2)
            cy3, cx3 = ndi.center_of_mass(np.clip(cut - np.median(cut), 0, None) * (np.hypot(*np.mgrid[-11:12, -11:12]) < 6))
            stackm.append(ndi.shift(cut, (11 - cy3, 11 - cx3), order=1))
        m_ = np.mean(stackm, 0); m_ = m_ - np.median(m_[:4]); yy_, xx_ = np.mgrid[-11:12, -11:12]; rr_ = np.hypot(yy_, xx_)
        r_ = np.arange(0, 9); prof = np.array([m_[(rr_ >= r) & (rr_ < r + 1)].mean() for r in r_]); half = prof[0] / 2
        hw = next((r_[i - 1] + (prof[i - 1] - half) / (prof[i - 1] - prof[i]) for i in range(1, len(prof)) if prof[i] < half), np.nan)
        fw.append((nm, 2 * hw * ARC)); 
    print('   moon (point-source) FWHM from radial half-max, arcsec: ' + ', '.join('%s %.1f' % x for x in fw) + '  -> median %.1f"' % np.nanmedian([x[1] for x in fw]))
    psf = np.nanmedian([x[1] for x in fw])

    # ---- disc size and limb sharpness from the drizzled stack, along the true equator / pole axes
    fr_j, res_j, order_j, hi_j = sets['jupiter']; im = np.clip(res_j['best20'][0], 0, None); c0 = HALF * S
    m = im > im.max() * 0.4; cy_, cx_ = ndi.center_of_mass(im * m)
    def ray(theta, rmax=26):
        r = np.arange(0, rmax * S, 0.5); xs = cx_ + r * np.cos(theta); ys = cy_ + r * np.sin(theta); return r / S, ndi.map_coordinates(im, [ys, xs], order=1)
    interior = np.mean([ray(np.radians(t))[1][ray(np.radians(t))[0] < 4].mean() for t in range(0, 180, 30)])
    def edge(theta):
        r, v = ray(theta); v = v / interior
        r50 = np.interp(0.5, v[::-1][np.argmax(v[::-1] > 0.5):][:2] if False else [0.5], [0.5]) if False else None
        i50 = next(i for i in range(len(v)) if r[i] > 3 and v[i] < 0.5); r50 = r[i50 - 1] + (v[i50 - 1] - 0.5) / (v[i50 - 1] - v[i50]) * (r[i50] - r[i50 - 1])
        i90 = next(i for i in range(len(v)) if r[i] > 3 and v[i] < 0.9); i10 = next(i for i in range(len(v)) if r[i] > 3 and v[i] < 0.1) if (v[r > 3] < 0.1).any() else None
        return r50, (r[i10] - r[i90]) if i10 else np.nan
    th_e = np.radians(eq); eqd = [edge(th_e + k) for k in (0, np.pi)]; pol = [edge(th_e + np.pi / 2 + k) for k in (0, np.pi)]
    D_eq = (eqd[0][0] + eqd[1][0]) * ARC; D_pol = (pol[0][0] + pol[1][0]) * ARC; ew = np.nanmean([e[1] for e in eqd + pol]) * ARC
    print('   Jupiter disc at 50%% of centre brightness: equatorial %.1f" (a true 34" limb-darkened disc reads ~31" with this method at 3.8" blur); the polar value (%.1f") is biased low by dark polar regions, so no flattening is quoted' % (D_eq, D_pol))
    # belts: pole-to-pole profile averaged across +-3 px along the equator
    L = 22 * S; ts = np.arange(-L, L + 1); prof = []
    for off in range(-3 * S, 3 * S + 1):
        xs = cx_ + ts * np.cos(th_e + np.pi / 2) + off * np.cos(th_e); ys = cy_ + ts * np.sin(th_e + np.pi / 2) + off * np.sin(th_e); prof.append(ndi.map_coordinates(im, [ys, xs], order=1))
    prof = np.mean(prof, 0) / interior; arc = ts / S * ARC
    print('   pole-to-pole profile (fraction of interior), offset arcsec -> value:  ' + '  '.join('%+.0f":%.2f' % (v, prof[int(np.argmin(np.abs(arc - v)))]) for v in range(-14, 15, 2)))
    inner = np.abs(arc) < 12; dd = [(arc[i], prof[i]) for i in range(1, len(arc) - 1) if inner[i] and prof[i] < prof[i - 1] and prof[i] <= prof[i + 1]]
    print('   dark minima inside the disc (offset, brightness): ' + ', '.join('%+.1f"(%.2f)' % d for d in dd) + ';  max-min contrast across the disc interior: %.0f%%' % (100 * (prof[inner].max() - prof[np.abs(arc) < 10].min()) / prof[inner].max()))
    report['jupiter'].update(equatorial_diameter_arcsec=float(D_eq), polar_diameter_arcsec=float(D_pol), moon_fwhm_arcsec=float(psf), equator_axis_deg=float(eq))
    # ---- Mars half-maximum size on the same footing
    fr_m, res_m, order_m, hi_m = sets['mars']; imm = np.clip(res_m['best20'][0], 0, None); c = ndi.center_of_mass(imm * (imm > imm.max() * 0.5))
    wd = []
    for t in np.arange(0, np.pi, np.pi / 12):
        r = np.arange(0, 14 * S, 0.5); v = ndi.map_coordinates(imm, [c[0] + r * np.sin(t), c[1] + r * np.cos(t)], order=1); v2 = ndi.map_coordinates(imm, [c[0] - r * np.sin(t), c[1] - r * np.cos(t)], order=1)
        pk_ = imm.max(); h1 = next(r[i] for i in range(len(v)) if v[i] < pk_ / 2); h2 = next(r[i] for i in range(len(v2)) if v2[i] < pk_ / 2); wd.append((h1 + h2) / S * ARC)
    print('\nMars half-maximum width (best 20%%, 6 frames): %.1f" (%.1f..%.1f); the disc alone is ~5.8", so this is BIASED LOW (clipped peak pixels in the sharpest frames) and is not quoted as a size' % (np.mean(wd), min(wd), max(wd)))
    report['mars']['half_max_width_arcsec'] = float(np.mean(wd))
    json.dump(report, open(os.path.join(a.out, 'planets_report.json'), 'w'), indent=1, default=float)
    json.dump(dict(rotation_offset_deg=float(best_ang), matched=int(best_score[0])), open(os.path.join(a.out, 'jupiter_moon_match.json'), 'w'))
    # annotated wide image + comparison sheet
    big = to8(np.clip(hp, 0, None), 12 * sg, 0.5); big = cv2.cvtColor(cv2.resize(big, (900, 900), interpolation=cv2.INTER_CUBIC), cv2.COLOR_GRAY2BGR); k = 900 / (2 * W)
    for nm, (pe, pn, mag) in pred.items():
        ang = np.deg2rad(orient + best_ang); R = np.array([[np.cos(ang), np.sin(ang)], [np.sin(ang), -np.cos(ang)]]); v = np.linalg.inv(R) @ np.array([-pe, -pn]) / SHh
        px, py = int((W + v[0]) * k), int((W + v[1]) * k)
        if 0 < px < 900 and 0 < py < 900: cv2.circle(big, (px, py), 14, (0, 200, 255), 1, cv2.LINE_AA); cv2.putText(big, nm, (px + 16, py - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 200, 255), 1, cv2.LINE_AA)
    cv2.putText(big, 'Jupiter and Galilean moons (+-530") - %d frames at 0.8 ms' % n, (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.imwrite(os.path.join(a.out, 'jupiter_moons_wide.png'), big)
    tiles = []
    for name in ('mars', 'jupiter'):
        fr_, res_, order_, hi_ = sets[name]; row = []
        for k_ in ('all', 'best50', 'best20'):
            im = res_[k_][0]; c = np.clip(im[HALF * S - 24 * S:HALF * S + 24 * S, HALF * S - 24 * S:HALF * S + 24 * S], 0, None); t8 = to8(c, hi_, 0.7)
            t8 = cv2.cvtColor(cv2.resize(t8, (360, 360), interpolation=cv2.INTER_CUBIC), cv2.COLOR_GRAY2BGR); cv2.putText(t8, '%s %s (%d f)' % (name, k_, res_[k_][1]), (6, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 0), 1, cv2.LINE_AA); row.append(t8)
        tiles.append(np.hstack(row))
    cv2.imwrite(os.path.join(a.out, 'planets_comparison.png'), np.vstack(tiles))
    # ---- colour composites and disc colour indices (white balance provisional)
    rows = []; print('\nDisc colour (centre 5 px radius, white-balanced so the median field star is neutral):')
    for name in ('mars', 'jupiter'):
        fr_, res_, order_, hi_ = sets[name]; rgb = colour(fr_, order_, SHIFTS[name], 0.5); c0 = CR // 2
        yy_, xx_ = np.mgrid[:rgb.shape[0], :rgb.shape[1]]; cc = ndi.center_of_mass(rgb[..., 1] * (rgb[..., 1] > rgb[..., 1].max() * 0.5)); m_ = np.hypot(yy_ - cc[0], xx_ - cc[1]) < 2.5
        R_, G_, B_ = (rgb[..., k][m_].mean() for k in range(3)); print('   %-8s R/G %.2f  B/G %.2f   (R:G:B = %.2f : 1 : %.2f)' % (name, R_ / G_, B_ / G_, R_ / G_, B_ / G_)); report[name]['rg'] = float(R_ / G_); report[name]['bg'] = float(B_ / G_)
        if name == 'jupiter': lo, hi2 = 0.30 * G_, 1.08 * max(R_, G_)
        else: lo, hi2 = 0.0, np.percentile(rgb[..., 1], 99.95)
        img = np.clip((rgb - lo) / (hi2 - lo), 0, 1) ** 0.8; box = img[c0 - 24:c0 + 24, c0 - 24:c0 + 24]
        t8 = cv2.resize((box * 255).astype(np.uint8), (480, 480), interpolation=cv2.INTER_CUBIC)[..., ::-1].copy()
        cv2.putText(t8, '%s: sharpest %d of %d frames, provisional colour' % (name.capitalize(), max(3, len(fr_) // 2), len(fr_)), (6, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
        rows.append(t8); cv2.imwrite(os.path.join(a.out, f'{name}_colour.png'), t8)
    cv2.imwrite(os.path.join(a.out, 'planets_colour.png'), np.hstack(rows))
    json.dump(report, open(os.path.join(a.out, 'planets_report.json'), 'w'), indent=1, default=float)
    print('\nwrote', a.out)

if __name__ == '__main__':
    main()
