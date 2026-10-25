#!/usr/bin/env python3
"""Daytime Moon from a burst of raw 16-bit Origin FITS frames: sky-subtracted colour stack plus geometry checks.

    tools/moon.py --frames 'build/moon/burst2/*.fits' --out ~/Pictures/CelestronStacks/moon

The daytime sky (about 46,000 ADU) is far brighter than the crescent's limb contribution, so each channel has a smooth sky model
(masked Gaussian, Moon excluded) subtracted before frames are registered by phase correlation on the limb and averaged.
Checks: fitted limb radius vs the ephemeris angular size, crescent width vs the lit fraction, lit-limb direction vs the Sun's
position angle (a test of the image orientation from ORIENTAT).
"""
import argparse, glob, os, sys
from datetime import datetime, timedelta
import cv2, numpy as np
from astropy.io import fits
from scipy import ndimage as ndi
from skimage.registration import phase_cross_correlation

BL = 4985.0
ARC = 0.000410465 * 3600
WB = (1 / 0.65, 1.0, 1 / 0.67)      # provisional white balance (median field star neutral); order R, G, B

def channels(d):
    """half-resolution R,G,B from the GRBG array: R=(0,1), G=mean of (0,0),(1,1), B=(1,0); white balanced"""
    d = d.astype(np.float32) - BL
    return np.dstack([d[0::2, 1::2] * WB[0], (d[0::2, 0::2] + d[1::2, 1::2]) / 2 * WB[1], d[1::2, 0::2] * WB[2]])

def sky_subtract(ch, mask, sigma=100):
    out = np.empty_like(ch); w = (~mask).astype(np.float32); den = ndi.gaussian_filter(w, sigma)
    for k in range(3): out[..., k] = ch[..., k] - ndi.gaussian_filter(ch[..., k] * w, sigma) / np.maximum(den, 1e-6)
    return out

def fit_circle(x, y):
    A = np.c_[2 * x, 2 * y, np.ones_like(x)]; b = x ** 2 + y ** 2; c = np.linalg.lstsq(A, b, rcond=None)[0]
    return c[0], c[1], np.sqrt(c[2] + c[0] ** 2 + c[1] ** 2)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--frames', required=True); ap.add_argument('--out', default=os.path.expanduser('~/Pictures/CelestronStacks/moon')); ap.add_argument('--reuse', action='store_true', help='reuse the cached stack'); a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True); fs = sorted(glob.glob(os.path.expanduser(a.frames))); print(len(fs), 'frames')
    h0 = fits.getheader(fs[0]); orient = float(np.mean([fits.getheader(f)['ORIENTAT'] for f in fs[::8]]))
    # --- ephemeris first: the Moon's angular size fixes the limb radius we search for
    from astropy.coordinates import EarthLocation, get_body
    from astropy.time import Time
    import astropy.units as u
    t_mid = datetime.strptime(h0['DATE-OBS'], '%Y-%m-%dT%H:%M:%S') - timedelta(hours=1) + timedelta(seconds=55); T = Time(t_mid)
    loc = EarthLocation(lat=52.2054 * u.deg, lon=0.1218 * u.deg, height=20 * u.m); moon = get_body('moon', T, loc); sun = get_body('sun', T, loc)
    dist = moon.distance.to(u.km).value; ang_r = np.degrees(np.arcsin(1737.4 / dist)) * 60; el = sun.separation(moon).deg; k = (1 - np.cos(np.radians(el))) / 2; pa_sun = moon.position_angle(sun).deg
    Rp = ang_r * 60 / (2 * ARC)                                                      # predicted limb radius, half-res px
    print('ephemeris at %s UTC: Moon distance %.0f km -> angular radius %.2f arcmin (= %.0f half-res px); Sun elongation %.1f deg -> %.1f%% lit' % (t_mid.strftime('%H:%M:%S'), dist, ang_r, Rp, el, 100 * k))
    c0 = channels(fits.getdata(fs[0])); g = c0[..., 1]; yy, xx = np.mgrid[:g.shape[0], :g.shape[1]]
    def hough(img, centres_x, centres_y, radii):
        best = (-1e18, 0, 0, 0); th = np.linspace(0, 2 * np.pi, 360, endpoint=False); X, Y = np.meshgrid(centres_x, centres_y); X = X.ravel(); Y = Y.ravel()
        for R in radii:
            px = X[:, None] + R * np.cos(th)[None]; py = Y[:, None] + R * np.sin(th)[None]; ok = (px > 0) & (px < img.shape[1] - 1) & (py > 0) & (py < img.shape[0] - 1)
            v = ndi.map_coordinates(img, [py.ravel().clip(0, img.shape[0] - 1), px.ravel().clip(0, img.shape[1] - 1)], order=1).reshape(px.shape) * ok
            sc = np.clip(v, 0, None).sum(1); i = int(np.argmax(sc))
            if sc[i] > best[0]: best = (sc[i], X[i], Y[i], R)
        return best
    def locate(img):
        coarse = hough(img, np.arange(60, 1470, 8), np.arange(60, 970, 8), Rp * np.array([0.97, 1.0, 1.03]))
        fine = hough(img, np.arange(coarse[1] - 10, coarse[1] + 11, 1.0), np.arange(coarse[2] - 10, coarse[2] + 11, 1.0), np.arange(coarse[3] - 20, coarse[3] + 21, 2.0))
        return fine[1], fine[2], fine[3]
    hp0 = ndi.gaussian_filter(g - ndi.gaussian_filter(g, 40), 2); cx, cy, R = locate(hp0)
    print('limb circle from frame 0 (half-res px): centre (%.0f, %.0f) radius %.1f' % (cx, cy, R))
    mask = np.hypot(xx - cx, yy - cy) < R + 45                                     # whole Moon excluded from the sky model
    y0, y1, x0, x1 = int(cy - R - 45), int(cy + R + 45), int(cx - R - 45), int(cx + R + 45); y0, x0 = max(y0, 0), max(x0, 0); y1, x1 = min(y1, g.shape[0]), min(x1, g.shape[1])
    # --- register every frame on the sky-subtracted green channel, stack
    sp = os.path.join(a.out, 'stack.npy')
    if a.reuse and os.path.exists(sp):
        stack = np.load(sp); print('reusing cached stack')
    else:
        ref = None; acc = np.zeros_like(c0); n = 0; shifts = []
        for f in fs:
            ch = sky_subtract(channels(fits.getdata(f)), mask); gs = ndi.gaussian_filter(ch[..., 1], 1.5)[y0:y1, x0:x1]
            if ref is None: ref = gs
            sh = phase_cross_correlation(ref, gs, upsample_factor=20, normalization=None)[0]; shifts.append(sh)
            acc += np.dstack([ndi.shift(ch[..., k], sh, order=3) for k in range(3)]); n += 1
        stack = acc / n; sh = np.array(shifts); np.save(sp, stack)
        print('drift over the burst: %.1f half-res px in total (%.2f px/s; the Moon moves ~0.55"/s against the stars)' % (np.hypot(*(sh[-1] - sh[0])), np.hypot(*(sh[-1] - sh[0])) / 110))
    # --- measurements on the stack
    g = ndi.gaussian_filter(stack[..., 1], 1.0); cx, cy, R = locate(ndi.gaussian_filter(stack[..., 1], 2))
    r = np.hypot(xx - cx, yy - cy); sg = 1.4826 * np.median(np.abs(g[r > R + 30] - np.median(g[r > R + 30])))
    print('limb circle from the stack: centre (%.1f, %.1f) radius %.1f half-res px -> angular radius %.2f arcmin; measured / predicted = %.3f' % (cx, cy, R, R * 2 * ARC / 60, R * 2 * ARC / 60 / ang_r))
    ang = np.degrees(np.arctan2(yy - cy, xx - cx)); ring = (r > R - 70) & (r < R + 12); lit = ring & (g > 0.35 * np.percentile(g[ring], 99.9))      # the bright ridge only
    mid = np.degrees(np.arctan2(np.mean(np.sin(np.radians(ang[lit]))), np.mean(np.cos(np.radians(ang[lit])))))
    th = np.arange(-90, 91, 5); span = [(d, ndi.map_coordinates(g, [[cy + (R - 8) * np.sin(np.radians(mid + d))], [cx + (R - 8) * np.cos(np.radians(mid + d))]], order=1)[0]) for d in th]
    litspan = [d for d, v in span if v > 6 * sg]; print('lit limb above 6 sigma spans %.0f deg of arc (cusps thin out toward +-90 deg)' % ((max(litspan) - min(litspan)) if litspan else 0))
    t = np.arange(R - 150, R + 25, 0.5); prof = ndi.map_coordinates(g, [cy + t * np.sin(np.radians(mid)), cx + t * np.cos(np.radians(mid))], order=1); prof = ndi.gaussian_filter1d(prof, 3)
    ipk = int(np.argmax(prof)); half = prof[ipk] / 2; lo = ipk
    while lo > 0 and prof[lo] > half: lo -= 1
    width = (t[ipk:][np.argmax(prof[ipk:] < half)] - t[lo]) if (prof[ipk:] < half).any() else np.nan
    geo = R * (1 - abs(np.cos(np.radians(180 - el))))
    print('crescent at its midpoint: brightness falls to half in %.0f half-res px (%.0f"); full geometric width at this phase %.0f px (%.0f"); limb peak %.0f ADU in the stack (noise %.0f ADU)' % (width, width * 2 * ARC, geo, geo * 2 * ARC, prof.max(), sg))
    rr = np.deg2rad(orient); Rm = np.array([[np.cos(rr), np.sin(rr)], [np.sin(rr), -np.cos(rr)]]); v = Rm @ np.array([np.cos(np.radians(mid)), np.sin(np.radians(mid))]); pa_meas = (np.degrees(np.arctan2(-v[0], -v[1]))) % 360
    print('orientation: Sun position angle from the Moon %.1f deg; lit-limb midpoint derotated with ORIENTAT %.1f deg is %.1f deg -> difference %+.1f deg' % (pa_sun, orient, pa_meas, ((pa_meas - pa_sun + 180) % 360) - 180))
    ch1 = sky_subtract(channels(fits.getdata(fs[0])), mask)
    print('single-frame noise (green, 1 sigma) %.0f ADU -> stack %.0f ADU' % (np.std(ch1[..., 1][r > R + 30]), sg))
    # --- products: sky-subtracted colour crescent, hard-stretched
    crop = stack[y0:y1, x0:x1]; hi = np.percentile(crop[..., 1], 99.9); img = np.clip(crop / hi, 0, 1) ** 0.7
    cv2.imwrite(os.path.join(a.out, 'moon_crescent_colour.png'), (img * 255).astype(np.uint8)[..., ::-1])
    lum = 0.25 * crop[..., 0] + 0.5 * crop[..., 1] + 0.25 * crop[..., 2]; cv2.imwrite(os.path.join(a.out, 'moon_crescent_mono_16bit.png'), (np.clip(lum / lum.max(), 0, 1) * 65535).astype(np.uint16))
    # one raw frame for comparison (gradient removed, same stretch rule)
    cv2.imwrite(os.path.join(a.out, 'moon_single_frame.png'), (np.clip(ch1[y0:y1, x0:x1] / hi, 0, 1) ** 0.7 * 255).astype(np.uint8)[..., ::-1])
    print('wrote', a.out)

if __name__ == '__main__':
    main()
