"""Background model for a target that fills the frame: sky samples are taken only from outside a band along the target's major axis (the flow's generic model fitted the galaxy's faint halo as sky and cut it off),
then the same TGV denoise as the flow. Writes processed_linear.fits for render_variants.py.

    bg_halo.py --stack integrated.fits --wcs wcs.json --report report.json --ra 10.6847 --dec 41.2687 --pa 38 --halfwidth-arcmin 22 --degree 2 --out DIR
"""
import argparse, json, os, sys
import numpy as np
from astropy.io import fits
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assess, flow

ap = argparse.ArgumentParser()
for k in ('stack', 'wcs', 'report', 'out'): ap.add_argument('--' + k, required=True)
ap.add_argument('--ra', type=float, required=True); ap.add_argument('--dec', type=float, required=True); ap.add_argument('--pa', type=float, required=True)
ap.add_argument('--halfwidth-arcmin', type=float, default=22); ap.add_argument('--degree', type=int, default=2)
a = ap.parse_args(); work = os.path.join(a.out, 'work'); os.makedirs(work, exist_ok=True)
w = json.load(open(a.wcs)); rep = json.load(open(a.report))
d, _ = assess.load_stack(a.stack); valid = assess.valid_mask(d); C, H, W = d.shape
cx, cy = flow.world2pix(w, a.ra, a.dec)
x0, y0 = flow.world2pix(w, a.ra + 0.2 * np.sin(np.radians(a.pa)) / np.cos(np.radians(a.dec)), a.dec + 0.2 * np.cos(np.radians(a.pa)))
ux, uy = np.array([x0 - cx, y0 - cy]) / np.hypot(x0 - cx, y0 - cy)
yy, xx = np.mgrid[0:H, 0:W]; across = np.abs((xx - cx) * (-uy) + (yy - cy) * ux) * assess.ARCSEC_PER_PX / 60
band = across < a.halfwidth_arcmin
print('target at (%.0f, %.0f), major axis direction (%.2f, %.2f); %.0f%% of the frame outside the band' % (cx, cy, ux, uy, 100 * (~band & valid).sum() / valid.sum()))
bg, rms = assess.fit_background(d, valid & ~band, a.degree)
d_bg = d - bg; d_bg += 0.01 - np.median(d_bg[:, valid & ~band], axis=1)[:, None, None]; d_bg[:, ~valid] = 0
bgpath = os.path.join(work, 'bgsub.fits'); fits.PrimaryHDU(np.clip(d_bg, 0, 1).astype(np.float32)).writeto(bgpath, overwrite=True)
chosen, rows = flow.tgv_search(bgpath, assess.hp_noise(d_bg.mean(0), valid), valid, rep['plan']['steps']['denoise'], work)
print('TGV strength %s noise x%.2f' % (chosen[0], chosen[2]))
fits.writeto(os.path.join(a.out, 'processed_linear.fits'), chosen[1].astype(np.float32), overwrite=True)
fits.writeto(os.path.join(a.out, 'pre_tgv.fits'), d_bg.astype(np.float32), overwrite=True)
