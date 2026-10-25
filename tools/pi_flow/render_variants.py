"""Render variants of a processed linear stack (processed_linear.fits from flow.py) for judging depth and realism: black point, display level, multi-scale local contrast, texture put
back from the undenoised stack, colour with and without the SPCC factors. Output: 16-bit PNGs plus an 8-bit contact sheet.

    render_variants.py --linear processed_linear.fits --pre pre_tgv.fits --gains R G B --out DIR
"""
import argparse, math, os
import numpy as np, cv2
from astropy.io import fits
from scipy import ndimage as ndi
import assess

VARIANTS = {
    'B_deeper':        dict(black=1.5, level=0.20, ms=((8, .45), (25, .45), (80, .35)), texture=0.0, spcc=True,  sat=1.8),
    'C_texture':       dict(black=1.5, level=0.20, ms=((8, .45), (25, .45), (80, .35)), texture=0.45, spcc=True,  sat=1.8),
    'D_uncalibrated':  dict(black=1.5, level=0.20, ms=((8, .45), (25, .45), (80, .35)), texture=0.0, spcc=False, sat=1.8),
    'F_finished':      dict(black=1.0, level=0.25, ms=((8, .45), (25, .45), (80, .35)), texture=0.0, spcc=True,  sat=1.8, armblue=0.18, crop=True),
    'G_lanes1':        dict(black=1.0, level=0.25, ms=((8, .3),), texture=0.0, spcc=True, sat=1.8, armblue=0.18, crop=True, lanes=(1.0, 40)),
    'G_lanes2':        dict(black=1.0, level=0.25, ms=((8, .3),), texture=0.0, spcc=True, sat=1.8, armblue=0.18, crop=True, lanes=(2.0, 40)),
    'H_lanes25':       dict(black=1.0, level=0.25, ms=((8, .3),), texture=0.0, spcc=True, sat=1.8, armblue=0.18, crop=True, lanes=(2.5, 25)),
    'H_lanes4':        dict(black=1.0, level=0.25, ms=((8, .3),), texture=0.0, spcc=True, sat=1.8, armblue=0.18, crop=True, lanes=(4.0, 25)),
    'E_dramatic':      dict(black=2.0, level=0.12, ms=((8, .6), (25, .6), (80, .5)),    texture=0.3, spcc=True,  sat=2.2),
}

def render(lin, pre, gains, v, valid):
    d = lin + v['texture'] * (pre - lin) if v['texture'] else lin
    g = np.array(gains if v['spcc'] else [1, 1, 1], float); x = np.clip(d * (g / g.mean())[:, None, None], 0, None); x[:, ~valid] = 0; L = x.mean(0)
    sky = float(np.median(L[valid])); noise = assess.hp_noise(L, valid); Lb = np.clip(L - (sky - v['black'] * noise), 0, None)
    if v.get('lanes'):                       # flatten the smooth bulge/disk light so that lanes and arms carry the contrast: log-domain detail of the starless luminance against its large-scale model, applied as a gain
        gain, sig = v['lanes']; Lsl = ndi.median_filter(Lb, size=15); k = 4 * noise
        detail = np.log(ndi.gaussian_filter(Lsl, 2.5) + k) - np.log(ndi.gaussian_filter(Lsl, sig) + k);       # band between 2.5 px and sig px only: the large-scale light profile (bulge to disk) is kept as it is
        wt = np.clip((ndi.gaussian_filter(Lb, 3) - noise) / (4 * noise), 0, 1); detail = np.clip(detail, -0.8, 0.25)      # bounded, so that the bright core and star cores cannot set the display range
        Lb = Lb * np.exp(gain * detail * wt); x = x * np.exp(gain * detail * wt)[None]
    ref = max(float(np.percentile(ndi.gaussian_filter(Lb, 3.0)[valid], 85)), 3 * noise); xmax = float(Lb.max()) if not v.get('lanes') else float(np.percentile(Lb[valid], 99.999)); lo, hi = 1e-5, 10.0
    for _ in range(60):
        s = math.sqrt(lo * hi)
        if math.asinh(ref / s) / math.asinh(xmax / s) < v['level']: hi = s
        else: lo = s
    Ls = np.arcsinh(Lb / s) / math.asinh(xmax / s)
    xs = np.stack([ndi.gaussian_filter(x[c], 2.0) for c in range(3)]); ratio = xs / np.clip(xs.mean(0), 1e-6, None)
    rgb = np.clip(1 + (ratio - 1) * v['sat'], 0, 3); fade = np.clip((ndi.gaussian_filter(L, 2.0) - 0.5 * noise) / (3 * noise), 0, 1); rgb = 1 + (rgb - 1) * fade
    if v.get('armblue'):                                                                  # taste adjustment, not a measurement: bluer away from the old-star core, none at the core
        Lsm = ndi.gaussian_filter(Ls, 12); w = np.clip(1 - Lsm / max(float(np.percentile(Lsm[valid], 99.5)), 1e-6) * 1.6, 0, 1) * fade
        rgb = rgb * np.stack([1 - 0.5 * v['armblue'] * w, np.ones_like(w), 1 + v['armblue'] * w])
    out = rgb * Ls; lum = out.mean(0); loc = lum.copy()
    for size, amt in v['ms']: loc = loc + amt * (lum - ndi.gaussian_filter(lum, size)) / len(v['ms']) * 2       # each scale adds its band of detail
    out = np.clip(out * (np.clip(loc, 1e-4, None) / np.clip(lum, 1e-4, None)), 0, 1); out[:, ~valid] = 0
    img = np.ascontiguousarray(out.transpose(1, 2, 0)[::-1]).astype(np.float32)
    if v.get('crop'):
        ys, xs_ = np.nonzero(valid[::-1]); img = img[ys.min() + 8:ys.max() - 8, xs_.min() + 8:xs_.max() - 8]
    return img

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--linear', required=True); ap.add_argument('--pre', required=True); ap.add_argument('--gains', type=float, nargs=3, required=True); ap.add_argument('--out', required=True); ap.add_argument('--only')
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    lin = fits.getdata(a.linear).astype(np.float64); pre = fits.getdata(a.pre).astype(np.float64); valid = assess.valid_mask(lin, 4)
    sheet = []
    only = a.only.split(',') if a.only else list(VARIANTS)
    for name, v in ((n, VARIANTS[n]) for n in only):
        img = render(lin, pre, a.gains, v, valid); cv2.imwrite(os.path.join(a.out, name + '.png'), cv2.cvtColor((img * 65535 + .5).astype(np.uint16), cv2.COLOR_RGB2BGR))
        sheet.append(cv2.resize(img, (img.shape[1] // 3, img.shape[0] // 3), interpolation=cv2.INTER_AREA)); print(name, flush=True)
    if len(sheet) < 4: return
    top = np.hstack(sheet[:2]); bot = np.hstack(sheet[2:4]); cv2.imwrite(os.path.join(a.out, 'sheet.png'), cv2.cvtColor((np.vstack([top, bot]) * 255 + .5).astype(np.uint8), cv2.COLOR_RGB2BGR))

if __name__ == '__main__': main()
