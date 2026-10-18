"""Tune the render of a processed linear stack to match a survey reference of the same field (a hips2fits cutout on the same pixel grid): the tone curve (black point, arcsinh scale, gain) is fitted to the
reference's starless luminance, the disk colour is matched to the reference, and the small-scale (lane) contrast is scaled to the reference's. The numbers it fits are written next to the picture.
The reference is a different instrument and a different stretch, so this matches appearance, not photometry.

    match_survey.py --linear processed_linear.fits --gains R G B --ref dss2.png --out DIR [--sat 1.8]
"""
import argparse, json, os, sys
import numpy as np, cv2
from astropy.io import fits
from scipy import ndimage as ndi
from scipy.optimize import least_squares
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assess

ap = argparse.ArgumentParser(); ap.add_argument('--linear', required=True); ap.add_argument('--gains', type=float, nargs=3, required=True); ap.add_argument('--ref', required=True); ap.add_argument('--out', required=True)
ap.add_argument('--sat', type=float, default=1.8); ap.add_argument('--no-lanes', action='store_true'); ap.add_argument('--no-colour', action='store_true'); ap.add_argument('--tag', default='matched'); ap.add_argument('--lane-ref', help='survey image (same grid) whose small-scale contrast sets the lane gain; default: the tone/colour reference')
a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
lin = fits.getdata(a.linear).astype(np.float64); valid = assess.valid_mask(lin, 4); C, H, W = lin.shape
g = np.array(a.gains); x = np.clip(lin * (g / g.mean())[:, None, None], 0, None); x[:, ~valid] = 0; L = x.mean(0); noise = assess.hp_noise(L, valid)
ref = cv2.cvtColor(cv2.imread(a.ref), cv2.COLOR_BGR2RGB).astype(np.float64) / 255                # orientation of the render: row 0 = top = FITS row H-1
L = L[::-1]; x = x[:, ::-1]; valid = valid[::-1]
def starless(im): return ndi.gaussian_filter(ndi.median_filter(im, size=15), 3.0)
Lm = starless(L); Yr = ref.mean(2); Yrm = starless(Yr)
# --- tone curve: reference luminance ~ gain * asinh((L - b) / s) over the galaxy and halo, where the reference is not burnt out
fitm = valid & (Yrm < 0.9) & (ndi.binary_erosion(valid, iterations=60))
ys, xs = np.nonzero(fitm); sel = slice(None, None, 37); p = Lm[ys, xs][sel]; q = Yrm[ys, xs][sel]; sky = float(np.median(Lm[valid]))
def model(par, v): b, ls, gain = par; return gain * np.arcsinh(np.clip(v - b, 0, None) / np.exp(ls))
r = least_squares(lambda par: model(par, p) - q, [sky, np.log(10 * noise), 0.2], loss='soft_l1', f_scale=0.03, x_scale=[noise, 1, 0.1])
b, ls, gain = r.x; s = float(np.exp(ls)); print('tone fit: black %.5f (sky %.5f, noise %.5f), arcsinh scale %.5f, gain %.3f, rms residual %.3f' % (b, sky, noise, s, gain, float(np.sqrt(np.mean((model(r.x, p) - q) ** 2)))))
Lb = np.clip(L - b, 0, None); knee = 0.65
tone = lambda v: (lambda t: np.where(t < knee, t, knee + (1 - knee) * np.tanh((t - knee) / (1 - knee))))(gain * np.arcsinh(np.clip(v - b, 0, None) / s))
Y = tone(L)        # soft shoulder: the reference's burnt core is not copied, the bulge keeps a gradient
# --- colour: ratios from smoothed channels, saturation, then the disk's mean colour pulled to the reference's
xsl = np.stack([starless(x[c]) for c in range(3)]); ratio = xsl / np.clip(xsl.mean(0), 1e-6, None)               # the galaxy's own light, stars taken out: its colour is matched to the reference
fade = np.clip((Lm - 0.5 * noise) / (3 * noise), 0, 1); rgb = 1 + (np.clip(1 + (ratio - 1) * a.sat, 0, 3) - 1) * fade
xst = np.clip(x - xsl, 0, None); xsm = np.stack([ndi.gaussian_filter(xst[c], 1.6) for c in range(3)]); stl = xsm.mean(0); rst = xsm / np.clip(stl, 1e-9, None); sfade = np.clip((stl - 10 * noise) / (30 * noise), 0, 1)       # faint stars: too little signal to give a colour once the galaxy light is removed, so they stay neutral
rgb_st = 1 + (np.clip(1 + (rst - 1) * a.sat, 0, 3) - 1) * sfade                                                   # the stars keep their SPCC colour
starmask = ndi.gaussian_filter(ndi.binary_dilation((ndi.gaussian_filter(L, 1.0) - Lm) > 8 * noise, iterations=3).astype(float), 1.5); w = np.clip((Lm - b) / (10 * noise), 0, 1)             # stars are rendered separately, so the galaxy's colour correction applies everywhere, under the stars too
Ym = np.clip(tone(Lm), 0, 1); corr = []
if not a.no_colour:
    # colour matched as a function of brightness: the reference's disk goes from orange in the bulge to blue in the outskirts, so one global factor would be wrong
    edges = np.linspace(0.08, 0.95, 10); mid = 0.5 * (edges[1:] + edges[:-1]); F = np.ones((len(mid), 3)); regm = fitm & (starmask < 0.05)
    refs = np.stack([ndi.gaussian_filter(ref[..., c], 4) for c in range(3)]); mines = np.stack([ndi.gaussian_filter(rgb[c] * Ym, 4) for c in range(3)])
    for i in range(len(mid)):
        m_ = regm & (Ym >= edges[i]) & (Ym < edges[i + 1])
        if m_.sum() < 2000: F[i] = F[i - 1] if i else 1; continue
        rr = np.array([np.median(refs[c][m_]) for c in range(3)]); mm = np.array([np.median(mines[c][m_]) for c in range(3)]); F[i] = (rr / rr[1]) / (mm / mm[1])
    print('colour factors R/G/B by brightness bin %s:\n%s' % (np.round(mid, 2), np.round(F, 2)))
    fmap = np.stack([np.interp(Ym, mid, F[:, c]) for c in range(3)]); corr = F.tolist()
    rgb = rgb * (1 + (fmap - 1) * w[None])
out = rgb * Ym + rgb_st * np.clip(Y - Ym, 0, None)
# --- lane contrast: scale the 3 to 15 px band of the (starless) log luminance to the reference's
rep = {'knee': knee, 'black': b, 'arcsinh_scale': s, 'gain': gain, 'noise': noise, 'colour_factors': corr}
if not a.no_lanes:
    def band(im): sl = ndi.median_filter(np.clip(im, 1e-3, None), size=9); return np.log(ndi.gaussian_filter(sl, 2.0)) - np.log(ndi.gaussian_filter(sl, 15.0))
    reg = fitm & (Yrm > 0.1) & (Yrm < 0.7) & (starmask < 0.02);       # stars masked: both images' band contrast should be the galaxy's lanes, not star halos
    bm = band(out.mean(0)); LR = cv2.cvtColor(cv2.imread(a.lane_ref), cv2.COLOR_BGR2RGB).astype(np.float64).mean(2) / 255 if a.lane_ref else Yr; br = band(LR); sm_, sr_ = float(np.std(bm[reg])), float(np.std(br[reg])); u, v_ = bm[reg] - bm[reg].mean(), br[reg] - br[reg].mean(); corr_ = float((u * v_).sum() / np.sqrt((u * u).sum() * (v_ * v_).sum())); slope = float((u * v_).sum() / (u * u).sum())
    k = float(np.clip(slope, 1.0, 3.0))       # regression slope of the reference's band on mine: reference noise (uncorrelated with my lanes) does not inflate it, unlike a ratio of rms values
    print('lane band (3 to 15 px) rms of log luminance: mine %.3f, reference %.3f, correlation %.2f, regression slope %.2f -> detail gain %.2f' % (sm_, sr_, corr_, slope, k))
    bw = np.clip((ndi.gaussian_filter(Lb, 3) - noise) / (4 * noise), 0, 1) * (1 - np.clip(starmask * 3, 0, 1)) * ndi.gaussian_filter(ndi.binary_erosion(valid, iterations=40).astype(float), 10); out = out * np.exp((k - 1) * np.clip(bm, -0.8, 0.25) * bw)[None]; rep['lane_gain'] = k; rep['lane_rms'] = [sm_, sr_]
out = np.clip(out, 0, 1); out[:, ~valid] = 0
img = np.ascontiguousarray(out.transpose(1, 2, 0)).astype(np.float32); ys_, xs2 = np.nonzero(valid); img = img[ys_.min() + 8:ys_.max() - 8, xs2.min() + 8:xs2.max() - 8]
cv2.imwrite(os.path.join(a.out, a.tag + '.png'), cv2.cvtColor((img * 65535 + .5).astype(np.uint16), cv2.COLOR_RGB2BGR)); json.dump(rep, open(os.path.join(a.out, a.tag + '.json'), 'w'), indent=1, default=float)
S = os.path.join(a.out, a.tag + '_sheet.jpg'); r8 = (ref * 255).astype(np.uint8); m8 = np.zeros_like(r8); h_, w_ = img.shape[:2]; m8[8:8 + h_, 8:8 + w_] = (img * 255 + .5).astype(np.uint8)
sm = lambda t: cv2.resize(t, (1000, 670)); cv2.imwrite(S, cv2.cvtColor(np.hstack([sm(r8), sm(m8)]), cv2.COLOR_RGB2BGR))
if os.environ.get('DBG'):
    pk = (ndi.maximum_filter(Y - Ym, 9) == (Y - Ym)) & (Yrm > 0.3) & (Yrm < 0.7) & valid & ((Y - Ym) > 0.03); ys_, xs3 = np.nonzero(pk)
    for i in range(0, min(len(ys_), 400), 80):
        yy_, xx_ = ys_[i], xs3[i]; print((yy_, xx_), 'x', np.round(x[:, yy_, xx_] / x[:, yy_, xx_].mean(), 2), 'xsl', np.round(xsl[:, yy_, xx_] / xsl[:, yy_, xx_].mean(), 2), 'xst', np.round(xst[:, yy_, xx_], 5), 'rgb_st', np.round(rgb_st[:, yy_, xx_], 2), 'rgb', np.round(rgb[:, yy_, xx_], 2), 'out', np.round(out[:, yy_, xx_], 2), 'Y,Ym', round(float(Y[yy_, xx_]), 2), round(float(Ym[yy_, xx_]), 2))
