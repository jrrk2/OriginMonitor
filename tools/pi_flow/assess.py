"""Data-condition metrics for a linear RGB stack, and the target classes the processing recipes key on.

Everything here is measured from the stack itself (plus the target name): nothing assumes a particular night.
"""
import math, re
import numpy as np
from astropy.io import fits
from scipy import ndimage as ndi

ARCSEC_PER_PX = 1.478

# target class, apparent size (arcmin, long axis), faintness 0 (bright) .. 1 (faint). Same idea as the planner's table; names are matched on a normalised key.
TARGETS = {
    'm1': ('snr', 6, 0.0), 'm27': ('planetary_nebula', 8, 0.0), 'm57': ('planetary_nebula', 3, 0.0), 'm31': ('galaxy', 180, 0.3), 'm33': ('galaxy', 70, 0.69), 'm81': ('galaxy', 25, 0.23),
    'm82': ('galaxy', 11, 0.3), 'm101': ('galaxy', 25, 0.96), 'm51': ('galaxy', 11, 0.3), 'm63': ('galaxy', 12, 0.4), 'm45': ('reflection_nebula', 110, 0.0), 'm13': ('globular_cluster', 20, 0.0),
    'm92': ('globular_cluster', 14, 0.0), 'm3': ('globular_cluster', 18, 0.0), 'm15': ('globular_cluster', 12, 0.0), 'm42': ('emission_nebula', 65, 0.0), 'm8': ('emission_nebula', 90, 0.1),
    'm16': ('emission_nebula', 35, 0.3), 'm17': ('emission_nebula', 20, 0.2), 'm20': ('emission_nebula', 28, 0.3), 'ngc7000': ('emission_nebula', 120, 1.0), 'ic5070': ('emission_nebula', 60, 1.0),
    'ngc1499': ('emission_nebula', 150, 1.0), 'ic1396': ('emission_nebula', 170, 1.0), 'ic405': ('emission_nebula', 37, 1.0), 'ic1805': ('emission_nebula', 60, 1.0), 'ngc2174': ('emission_nebula', 40, 1.0),
    'ngc6992': ('emission_nebula', 60, 1.0), 'ngc869': ('open_cluster', 30, 0.0), 'ngc884': ('open_cluster', 30, 0.0), 'doublecluster': ('open_cluster', 60, 0.0),
}

def key_of(name):
    return re.sub(r'[^a-z0-9]', '', name.lower().replace('messier', 'm'))

def classify(name):
    k = key_of(name)
    for tk, v in TARGETS.items():
        if k == tk or k.startswith(tk) and not k[len(tk):len(tk) + 1].isdigit():
            return v
    if 'galaxy' in k: return ('galaxy', 20, 0.6)
    if 'cluster' in k: return ('open_cluster', 30, 0.2)
    if 'nebula' in k: return ('emission_nebula', 60, 0.8)
    return ('unknown', 20, 0.5)


def load_stack(path, swap_rb=False):
    d = fits.getdata(path).astype(np.float64); h = fits.getheader(path)
    if swap_rb: d = d[[2, 1, 0]]
    return d, h


def valid_mask(d, border=30):
    return ndi.binary_erosion((d > 0).all(0), iterations=border)


def _basis(X, Y, deg):
    return np.stack([X ** i * Y ** j for i in range(deg + 1) for j in range(deg + 1 - i)], 1)


def fit_background(d, valid, degree=3, bs=64):
    """per-channel polynomial sky model from sigma-clipped low-percentile block samples (stars and nebulosity sit above the 35th percentile). Returns the model (3,H,W) and the fit residual rms per channel."""
    C, H, W = d.shape; yy, xx = np.mgrid[0:H, 0:W]; model = np.zeros_like(d); rms = []
    Xg = ((xx - W / 2) / W).ravel(); Yg = ((yy - H / 2) / H).ravel()
    for c in range(C):
        xs, ys, vs = [], [], []
        for y in range(0, H - bs, bs):
            for x in range(0, W - bs, bs):
                if valid[y:y + bs, x:x + bs].mean() < 0.95: continue
                xs.append(x + bs / 2); ys.append(y + bs / 2); vs.append(np.percentile(d[c, y:y + bs, x:x + bs], 35))
        xs, ys, vs = map(np.array, (xs, ys, vs)); X = (xs - W / 2) / W; Y = (ys - H / 2) / H; keep = np.ones(len(vs), bool)
        for _ in range(4):
            coef = np.linalg.lstsq(_basis(X[keep], Y[keep], degree), vs[keep], rcond=None)[0]
            r = vs - _basis(X, Y, degree) @ coef; s = 1.4826 * np.median(np.abs(r[keep] - np.median(r[keep]))); keep = r < 1.5 * s
        model[c] = (_basis(Xg, Yg, degree) @ coef).reshape(H, W); rms.append(float(np.std(r[keep])))
    return model, rms


def gradient_strength(d, valid):
    """how much of the sky variation a plane cannot explain: (rms of a degree-3 model minus a plane) / noise. Near 0: flat sky, a plane (or nothing) is enough."""
    m1, _ = fit_background(d, valid, degree=1); m3, _ = fit_background(d, valid, degree=3)
    L = d.mean(0); noise = hp_noise(L, valid)
    diff = (m3 - m1).mean(0)[valid]
    return float(np.std(diff) / max(noise, 1e-9)), float(np.ptp(m3.mean(0)[valid]) / max(noise, 1e-9))


def hp_noise(L, valid):
    """robust pixel noise: high-pass residual in blank sky (stars and nebulosity masked by a brightness cut)"""
    sm = ndi.gaussian_filter(L, 2.0); hp = (L - sm)
    bgm = ndi.zoom(ndi.median_filter(L[::4, ::4], size=25), 4, order=1)[:L.shape[0], :L.shape[1]]
    blank = valid & (np.abs(sm - bgm) < 3 * 1.4826 * np.median(np.abs(L - bgm)[valid]))
    v = hp[blank]
    return float(1.4826 * np.median(np.abs(v - np.median(v))))


def star_psf(d, valid, max_stars=1500, R=15):
    """empirical PSF: the median of isolated, unsaturated, sub-pixel centred stars. Returns (psf, fwhm_px, ellipticity, n_stars, peak positions of all detected stars, saturated fraction)."""
    C, H, W = d.shape; L = d.mean(0); Ls = ndi.gaussian_filter(L, 1.0)
    bg = ndi.zoom(ndi.median_filter(L[::4, ::4], size=25), 4, order=1)[:H, :W]
    allpk = (Ls == ndi.maximum_filter(Ls, size=15)) & valid & (Ls - bg > 0.02)
    sat_frac = float(((d.max(0) > 0.85) & allpk).sum() / max(allpk.sum(), 1))
    pk = (Ls == ndi.maximum_filter(Ls, size=21)) & valid & (Ls - bg > 0.05) & (Ls - bg < 0.4)
    ys, xs = np.nonzero(pk); yy, xx = np.mgrid[-R:R + 1, -R:R + 1]; rr = np.hypot(xx, yy); st = []
    for y, x in zip(ys, xs):
        if not (R + 3 <= y < H - R - 4 and R + 3 <= x < W - R - 4): continue
        c = L[y - R - 1:y + R + 2, x - R - 1:x + R + 2]
        if c.max() > 0.8 or (c == 0).any() or pk[y - R:y + R + 1, x - R:x + R + 1].sum() > 1: continue
        c = c - np.median(c[1:-1, 1:-1][(rr > 11) & (rr < 15)]); w = np.clip(c[1:-1, 1:-1], 0, None) * (rr < 8); t = w.sum()
        if t <= 0: continue
        cx = (w * xx).sum() / t; cy = (w * yy).sum() / t
        s = ndi.shift(c, (-cy, -cx), order=3)[1:-1, 1:-1]; st.append(s / s[rr < 9].sum())
        if len(st) >= max_stars: break
    if len(st) < 20: return None, float('nan'), float('nan'), len(st), int(allpk.sum()), sat_frac
    raw = np.median(np.array(st), 0); psf = np.clip(raw, 0, None); psf[rr > 11] = 0; psf /= psf.sum()
    star_psf.raw = raw                                  # unclipped, for judging ringing (negative wings)
    sx = math.sqrt((psf * xx ** 2).sum()); sy = math.sqrt((psf * yy ** 2).sum()); sg = math.sqrt((sx ** 2 + sy ** 2) / 2)
    ell = 1 - min(sx, sy) / max(sx, sy)
    return psf, 2.355 * sg, ell, len(st), int(allpk.sum()), sat_frac


def core_fwhm(d, valid, sel_positions=None):
    """FWHM of the median radial profile of isolated stars (the core), used to judge a deconvolution"""
    psf, f, e, n, _, _ = star_psf(d, valid)
    if psf is None: return float('nan'), float('nan'), 0
    R = psf.shape[0] // 2; yy, xx = np.mgrid[-R:R + 1, -R:R + 1]; rr = np.hypot(xx, yy); raw = star_psf.raw
    prof = np.array([raw[(rr >= b - 0.5) & (rr < b + 0.5)].mean() for b in range(0, R)])
    i = int(np.argmax(prof < prof[0] / 2)); fw = 2 * (i - 1 + (prof[i - 1] - prof[0] / 2) / (prof[i - 1] - prof[i])) if i > 0 else float('nan')
    wing_min = float(prof[3:].min() / prof[0])        # negative: a dark ring around the stars
    return fw, wing_min, n


def object_snr(d_bg, center_xy, radius_px, valid):
    """signal-to-noise of the target region at the scale of its structure: (median of the smoothed luminance in the target disk minus the sky) / (noise of the same smoothing in blank sky)"""
    L = d_bg.mean(0); sm = ndi.gaussian_filter(L, 3.0); H, W = L.shape; yy, xx = np.mgrid[0:H, 0:W]
    cx, cy = center_xy; disk = (np.hypot(xx - cx, yy - cy) < radius_px) & valid
    ring = (np.hypot(xx - cx, yy - cy) > 1.6 * radius_px) & (np.hypot(xx - cx, yy - cy) < 2.4 * radius_px) & valid
    if disk.sum() < 100 or ring.sum() < 100: return float('nan'), float('nan')
    sky = np.median(sm[ring]); sig = np.median(sm[disk]) - sky
    blank = valid & (np.abs(sm - np.median(sm[valid])) < 4 * 1.4826 * np.median(np.abs(sm - np.median(sm[valid]))[valid]))
    noise = 1.4826 * np.median(np.abs(sm[blank] - np.median(sm[blank])))
    return float(sig / max(noise, 1e-9)), float(sig)


def measure(d, valid):
    """all the condition metrics in one dict (and the PSF array, which is not JSON)"""
    L = d.mean(0); med = [float(np.median(d[c][valid])) for c in range(3)]
    grad_rms, grad_range = gradient_strength(d, valid)
    psf, fwhm, ell, n_psf, n_stars, sat = star_psf(d, valid)
    mp = valid.sum() / 1e6
    return {'sky_median_rgb': med, 'noise_sigma': hp_noise(L, valid), 'gradient_rms_over_noise': grad_rms, 'gradient_range_over_noise': grad_range, 'psf_fwhm_px': fwhm, 'psf_ellipticity': ell,
            'psf_stars': n_psf, 'stars_detected': n_stars, 'star_density_per_mpx': n_stars / mp, 'saturated_star_fraction': sat, 'valid_megapixels': mp}, psf
