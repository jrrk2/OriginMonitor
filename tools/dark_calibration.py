#!/usr/bin/env python3
"""Hot-pixel map and amp-glow template from the camera's factory darks, for SkyFrame (ORIGIN_DARK_CAL).

    tools/dark_calibration.py [--cal ~/Calibration/Origin178-*] [--out DATA/calibration/dark_cal.fits] [--hot 150]

The factory set (bias000N.fits / dark000N.fits / dark000N.json: ISO 100, 200, 2000 at 10-30 s, about 24 C, 2025-10-22) is read out MIRRORED left-right
relative to our snapshots: its 2000 hottest pixels sit +14400 ADU above their same-colour neighbours in a median of our frames at the mirrored position
and not at all at the same one (2026-11-24). Everything is mirrored here, in raw coordinates.

  * hot pixels: excess over the mean of the four same-colour neighbours (2 px away) above --hot ADU per 6000 ISO.s (an ISO 800 8 s frame is 6400),
    from every 16-bit dark scaled to that, and above 6 sigma of that dark's own excess; any one of the darks
  * amp glow: each dark minus its bias, smoothed (median of 32 px blocks of each colour plane, bilinear back to full size), less its own median, per ISO.s;
    the darks agree on about 0.027 ADU per ISO.s in the glowing corner (ISO 200 x 10 s: +54, ISO 2000 x 15 s: +848)

Writes one FITS, float32, 2 planes at full raw size: plane 1 the hot mask (0/1), plane 2 the glow in ADU per ISO.s.
"""
import argparse, glob, json, os
import numpy as np
from astropy.io import fits


def same_colour_excess(a):
    nb = np.zeros_like(a); nb[2:-2, 2:-2] = (a[:-4, 2:-2] + a[4:, 2:-2] + a[2:-2, :-4] + a[2:-2, 4:]) / 4
    e = a - nb; e[:2] = e[-2:] = 0; e[:, :2] = e[:, -2:] = 0
    return e


def smooth_glow(a, B=32):
    """per colour plane: 32 px block medians, bilinear to full size"""
    from scipy.ndimage import zoom
    out = np.zeros_like(a)
    for i in (0, 1):
        for j in (0, 1):
            p = a[i::2, j::2]; H, W = p.shape; h, w = H // B, W // B
            b = np.median(p[:h * B, :w * B].reshape(h, B, w, B).transpose(0, 2, 1, 3).reshape(h, w, -1), axis=2)
            f = zoom(b, (H / h, W / w), order=1)[:H, :W]
            out[i::2, j::2] = np.pad(f, ((0, H - f.shape[0]), (0, W - f.shape[1])), mode='edge')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    cals = sorted(glob.glob(os.path.expanduser('~/Calibration/Origin178-*')))
    ap.add_argument('--cal', default=cals[0] if cals else None)
    ap.add_argument('--out', default=os.path.join(os.environ.get('ORIGIN_DATA_DIR', '/mnt/fits-backup/claude'), 'calibration', 'dark_cal.fits'))
    ap.add_argument('--hot', type=float, default=150, help='hot-pixel threshold, ADU per 6000 ISO.s (default 150)')
    a = ap.parse_args()
    hot = None; glow_num = None; glow_w = 0
    for j in sorted(glob.glob(os.path.join(a.cal, 'dark0*.json'))):
        m = json.load(open(j))
        if m.get('bitDepth', 16) != 16: continue
        isos = m['iso'] * m['exposureTimeMS'] / 1000.0
        d = fits.getdata(os.path.join(a.cal, m['fileName'])).astype(np.float64)[:, ::-1]
        b = fits.getdata(os.path.join(a.cal, m['biasFileName'])).astype(np.float64)[:, ::-1]
        e0 = same_colour_excess(d); sig = 1.4826 * np.median(np.abs(e0[::4, ::4]))
        h = (e0 * (6000.0 / isos) > a.hot) & (e0 > 6 * sig)        # and well clear of this dark's own noise: scaled up, a short dark's noise crosses the threshold
        hot = h if hot is None else (hot | h)
        g = smooth_glow(d - b); g -= np.median(g)
        glow_num = g * isos if glow_num is None else glow_num + g * isos      # weighted by ISO.s: the long, high-gain darks show the glow best
        glow_w += isos * isos
        print(f"{m['fileName']}: ISO {m['iso']} {m['exposureTimeMS'] / 1000:.0f} s ({isos:.0f} ISO.s): noise {sig:.0f}, hot {h.sum()}, glow corner max {g.max():+.0f} ADU = {g.max() / isos:.4f}/ISO.s")
    glow = glow_num / glow_w
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    hdr = fits.Header(); hdr['COMMENT'] = 'plane 1: hot-pixel mask; plane 2: amp glow, ADU per ISO.s; raw coordinates of our snapshots (factory darks mirrored)'
    hdr['HOTTHR'] = (a.hot, 'ADU per 6000 ISO.s'); hdr['SOURCE'] = os.path.basename(a.cal)
    fits.PrimaryHDU(np.stack([hot.astype(np.float32), glow.astype(np.float32)]).astype('>f4'), header=hdr).writeto(a.out, overwrite=True)
    H, W = glow.shape; q = lambda y, x: glow[int(y * H) - 40:int(y * H) + 40, int(x * W) - 40:int(x * W) + 40].mean() * 6400
    print(f'{a.out}: {hot.sum()} hot pixels ({hot.mean():.2%}); glow at ISO 800 x 8 s: corners TL {q(.04, .04):+.0f} TR {q(.04, .96):+.0f} BL {q(.96, .04):+.0f} BR {q(.96, .96):+.0f} ADU')


if __name__ == '__main__':
    main()
