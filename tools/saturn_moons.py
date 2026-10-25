#!/usr/bin/env python3
"""Saturn lucky-imaging video with its moons, from raw 16-bit Origin FITS frames (EXPTIME 2 ms, mono-equalised Bayer).

Stages (cached under --work, rerunnable): crops -> calib -> windows -> iapetus -> render
    tools/saturn_moons.py --frames ~/Pictures/CelestronOrigin --work build/saturn_moons --out ~/Pictures/CelestronStacks

Method
  * Crop 300x300 raw px around Saturn's centroid in every frame; rate frames by Fourier high-frequency energy.
  * Derotate to north-up. Alt/az field rotation comes from the header ORIENTAT, corrected by a quadratic in time that
    keeps the ring axis at its JPL-predicted position angle (the header WCS is unsolved, so ORIENTAT drifts).
  * Saturn panel: per sliding window keep the sharpest frames, register them by FFT phase correlation, and drizzle all
    four Bayer planes (gain-equalised) onto a 2x grid with the per-frame rotation applied, then a light unsharp mask.
  * Moon panels: registered mean of more frames, glare suppressed with a median high-pass. A moon is labelled only if
    its predicted JPL Horizons position shows a peak that the point-reflected position does not.
"""
import argparse, glob, json, os, pickle, subprocess, sys, urllib.parse, urllib.request
from datetime import datetime, timedelta
from multiprocessing import Pool
import cv2, numpy as np
from astropy.io import fits
from scipy import ndimage as ndi
from skimage.registration import phase_cross_correlation

BL = 4985.0                                           # sensor pedestal
GAIN = np.array([[1.0, 1.268], [2.19, 0.991]], np.float32)   # per-Bayer-plane gain equalisation (measured on Saturn)
ARC = 0.000410465 * 3600                              # arcsec per raw pixel
SH = 2 * ARC                                          # arcsec per half-res pixel
C = 300                                               # raw crop size
NB = 128                                              # near-field panel, half-res px (+-189")
HALF = 60                                             # Saturn drizzle half-width, raw px
S = 2                                                 # drizzle scale
RING_PA = -3.25                                       # ring axis angle in a north-up image (JPL NP.ang 3.25 deg)
WIN, STEP = 240, 60
G = {}                                                # shared (fork) state


def bin2(d): return d.reshape(d.shape[0] // 2, 2, d.shape[1] // 2, 2).sum((1, 3))
def equalise(d):
    e = d.copy()
    for i in (0, 1):
        for j in (0, 1): e[i::2, j::2] *= GAIN[i, j]
    return e
def centroid(b):
    sm = ndi.gaussian_filter(b, 3); y, x = np.unravel_index(sm.argmax(), sm.shape)
    y0, x0 = max(y - 60, 0), max(x - 60, 0); m = b[y0:y + 61, x0:x + 61]; m = np.clip(m - m.max() * 0.25, 0, None)
    cy, cx = ndi.center_of_mass(m); return y0 + cy, x0 + cx
def rotmat(orient):
    r = np.deg2rad(orient); return np.array([[np.cos(r), np.sin(r)], [np.sin(r), -np.cos(r)]])   # src offset -> north-up offset
def northup(img, orient, cx, cy, size):
    R = rotmat(orient); A = np.linalg.inv(R); t = np.array([cx, cy]) - A @ np.array([size / 2, size / 2])
    return cv2.warpAffine(img, np.hstack([A, t[:, None]]), (size, size), flags=cv2.INTER_CUBIC | cv2.WARP_INVERSE_MAP)
def corr_angle(t, p): return -(np.polyval(p, t / 3600) - RING_PA)
def axis_angle(img, thr=0.15):
    m = img > img.max() * thr; yy, xx = np.nonzero(m); w = img[yy, xx]
    x0 = np.average(xx, weights=w); y0 = np.average(yy, weights=w)
    cxx = np.average((xx - x0) ** 2, weights=w); cyy = np.average((yy - y0) ** 2, weights=w); cxy = np.average((xx - x0) * (yy - y0), weights=w)
    return 0.5 * np.degrees(np.arctan2(2 * cxy, cxx - cyy))
def hfq(e, cy, cx):
    g = ndi.uniform_filter(e, 2); y = int(round(cy)); x = int(round(cx)); g = g[y - 48:y + 48, x - 48:x + 48]
    if g.shape != (96, 96): return 0.0
    g = g / g.max(); w = np.hanning(96)[:, None] * np.hanning(96)[None, :]
    F = np.abs(np.fft.fftshift(np.fft.fft2((g - g.mean()) * w))) ** 2; r = np.hypot(*np.mgrid[-48:48, -48:48])
    return float(F[(r > 10) & (r < 30)].sum() / F[r < 10].sum())
def utc(h): return datetime.strptime(h['DATE-OBS'], '%Y-%m-%dT%H:%M:%S') - timedelta(hours=1)    # header is local BST (+0100)


# ---------------------------------------------------------------- stage 1: crops
def _crop(f):
    try:
        h = fits.getheader(f)
        if h['EXPTIME'] != 0.002: return None
        d = fits.getdata(f); b = bin2(d.astype(np.float32) - BL); cy, cx = centroid(b); cy *= 2; cx *= 2
        y0 = int(round(cy)) - C // 2; x0 = int(round(cx)) - C // 2; y0 -= y0 % 2; x0 -= x0 % 2
        y0 = min(max(y0, 0), d.shape[0] - C); x0 = min(max(x0, 0), d.shape[1] - C)
        crop = d[y0:y0 + C, x0:x0 + C]; e = equalise(crop.astype(np.float32) - BL)
        return (os.path.basename(f), crop.astype(np.uint16), cy - y0, cx - x0, h['ORIENTAT'],
                (utc(h) - datetime(2026, 10, 7)).total_seconds(), hfq(e, cy - y0, cx - x0))
    except Exception:
        return None

def stage_crops(a):
    out = os.path.join(a.work, 'crops.npz')
    if os.path.exists(out): return
    fs = sorted(glob.glob(os.path.join(a.frames, 'snapshot_*.fits')))[:-1]      # newest may still be written
    print(len(fs), 'files'); 
    with Pool(10) as p: res = [r for r in p.imap(_crop, fs, chunksize=8) if r]
    res.sort(key=lambda r: r[0])
    np.savez(out, crops=np.array([r[1] for r in res]), cy=[r[2] for r in res], cx=[r[3] for r in res],
             orient=[r[4] for r in res], t=[r[5] for r in res], q=[r[6] for r in res], names=[r[0] for r in res])
    print(len(res), 'frames at 2 ms')


# ---------------------------------------------------------------- stage 2: orientation calibration
def stage_calib(a):
    out = os.path.join(a.work, 'calib_poly.npy')
    if os.path.exists(out): return
    Z = np.load(os.path.join(a.work, 'crops.npz')); crops, cy, cx, ori, t = Z['crops'], Z['cy'], Z['cx'], Z['orient'], Z['t']; N = len(crops)
    CH = 100; ang = []; tc = []
    for s in range(0, N - CH + 1, CH):
        W = np.mean([northup(equalise(crops[i].astype(np.float32) - BL), ori[i], cx[i], cy[i], 128) for i in range(s, s + CH, 2)], 0)
        ang.append(axis_angle(W)); tc.append(t[s:s + CH].mean())
    ang = np.degrees(np.unwrap(np.radians(np.array(ang) * 2)) / 2)
    p = np.polyfit(np.array(tc) / 3600, ang, 2); np.save(out, p)
    print('ring-axis drift %.1f -> %.1f deg over the night; quadratic fit rms %.2f deg' % (ang[0], ang[-1], (ang - np.polyval(p, np.array(tc) / 3600)).std()))


# ---------------------------------------------------------------- stage 3: per-window stacks
def drizzle(frames, shifts):
    n = G['crops'].shape[1] // 2; size = 2 * HALF * S; acc = np.zeros(size * size); wt = np.zeros(size * size)
    jj, ii = np.meshgrid(np.arange(n), np.arange(n))
    for i, sh in zip(frames, shifts):
        R = rotmat(G['ori'][i] + corr_angle(G['t'][i], G['poly'])); eq = equalise(G['crops'][i].astype(np.float32) - BL)
        for oy, ox in ((0, 0), (0, 1), (1, 0), (1, 1)):
            v = eq[oy::2, ox::2].ravel(); x = (2 * jj + ox - G['cx'][i]).ravel(); y = (2 * ii + oy - G['cy'][i]).ravel()
            dx = R[0, 0] * x + R[0, 1] * y + sh[1] * 2; dy = R[1, 0] * x + R[1, 1] * y + sh[0] * 2     # north-up offsets, raw px
            u = (dx + HALF) * S; w = (dy + HALF) * S; u0 = np.floor(u).astype(int); w0 = np.floor(w).astype(int); fu = u - u0; fw = w - w0
            for du, dw, wgt in ((0, 0, (1 - fu) * (1 - fw)), (1, 0, fu * (1 - fw)), (0, 1, (1 - fu) * fw), (1, 1, fu * fw)):
                uu = u0 + du; ww = w0 + dw; ok = (uu >= 0) & (uu < size) & (ww >= 0) & (ww < size); k = ww[ok] * size + uu[ok]
                acc += np.bincount(k, weights=(v * wgt)[ok], minlength=size * size); wt += np.bincount(k, weights=wgt[ok], minlength=size * size)
    return (acc / np.maximum(wt, 1e-6)).reshape(size, size)

def _window(s):
    try:
        crops = G['crops']; q = G['q']; t = G['t']; poly = G['poly']
        order = np.argsort(-q[s:s + WIN]) + s
        sat_idx = order[:max(12, WIN // 4)]; moon_idx = order[:int(WIN * 0.6)]
        # moon field (half-res, north-up), registered on Saturn
        B = np.array([northup(bin2(crops[i].astype(np.float32) - BL), G['ori'][i] + corr_angle(t[i], poly), G['cx'][i] / 2, G['cy'][i] / 2, NB) for i in moon_idx])
        r0 = slice(NB // 2 - 24, NB // 2 + 24); win = np.hanning(48)[:, None] * np.hanning(48)[None, :]
        def pcc(ref, im): return np.clip(phase_cross_correlation(ref[r0, r0] * win, im[r0, r0] * win, upsample_factor=20, normalization=None)[0], -3, 3)
        for k in range(1, len(B)): B[k] = ndi.shift(B[k], pcc(B[0], B[k]), order=3)
        Bm = B.mean(0)
        # Saturn: register the sharp subset on Saturn alone, then drizzle
        L = np.array([northup(bin2(crops[i].astype(np.float32) - BL), G['ori'][i] + corr_angle(t[i], poly), G['cx'][i] / 2, G['cy'][i] / 2, NB) for i in sat_idx])
        sh = [np.zeros(2)] + [pcc(L[0], L[k]) for k in range(1, len(L))]
        for it in range(2):                                           # re-register to the running mean
            ref = np.array([ndi.shift(L[k], sh[k], order=3) for k in range(len(L))]).mean(0); sh = [pcc(ref, L[k]) for k in range(len(L))]
        sat = drizzle(sat_idx, sh)
        tm = datetime(2026, 10, 7) + timedelta(seconds=float(t[moon_idx].mean()))
        return dict(s=s, time=tm.isoformat(), sat=sat.astype(np.float32), field=Bm.astype(np.float32), q=float(q[sat_idx].mean()),
                    qmed=float(np.median(q[s:s + WIN])), nsat=len(sat_idx), nmoon=len(moon_idx))
    except Exception:
        import traceback; return dict(s=s, error=traceback.format_exc())

def stage_windows(a):
    out = os.path.join(a.work, 'windows.pkl')
    if os.path.exists(out): return
    Z = np.load(os.path.join(a.work, 'crops.npz'))
    G.update(crops=Z['crops'], cx=Z['cx'], cy=Z['cy'], ori=Z['orient'], t=Z['t'], q=Z['q'], poly=np.load(os.path.join(a.work, 'calib_poly.npy')))
    starts = list(range(0, len(G['crops']) - WIN + 1, STEP)); print(len(starts), 'windows')
    with Pool(10) as p: res = p.map(_window, starts, chunksize=1)
    bad = [r for r in res if 'error' in r]
    if bad: print(bad[0]['error']); sys.exit(1)
    pickle.dump(res, open(out, 'wb'))


# ---------------------------------------------------------------- ephemeris (JPL Horizons, geocentric: no site sent)
BODIES = {'Saturn': '699', 'Mimas': '601', 'Enceladus': '602', 'Tethys': '603', 'Dione': '604', 'Rhea': '605', 'Titan': '606', 'Hyperion': '607', 'Iapetus': '608'}
def ephemeris(a, res):
    out = os.path.join(a.work, 'eph.json')
    if not os.path.exists(out):
        t0 = datetime.fromisoformat(res[0]['time']) - timedelta(minutes=30); t1 = datetime.fromisoformat(res[-1]['time']) + timedelta(minutes=30); eph = {}
        for n, i in BODIES.items():
            q = {'format': 'json', 'COMMAND': f"'{i}'", 'OBJ_DATA': "'NO'", 'MAKE_EPHEM': "'YES'", 'EPHEM_TYPE': "'OBSERVER'", 'CENTER': "'500@399'",
                 'START_TIME': f"'{t0:%Y-%m-%d %H:%M}'", 'STOP_TIME': f"'{t1:%Y-%m-%d %H:%M}'", 'STEP_SIZE': "'10 m'", 'QUANTITIES': "'1,9'", 'ANG_FORMAT': "'DEG'", 'CSV_FORMAT': "'YES'"}
            r = json.load(urllib.request.urlopen('https://ssd.jpl.nasa.gov/api/horizons.api?' + urllib.parse.urlencode(q), timeout=60))['result']
            rows = [l.split(',') for l in r[r.index('$$SOE') + 6:r.index('$$EOE')].strip().splitlines()]
            eph[n] = [(x[0].strip(), float(x[3]), float(x[4]), x[5].strip()) for x in rows]
        json.dump(eph, open(out, 'w'))
    return json.load(open(out))
def predict(eph, t):
    def interp(n):
        rows = eph[n]; ts = [datetime.strptime(r[0], '%Y-%b-%d %H:%M') for r in rows]
        k = max(0, min(len(ts) - 2, int((t - ts[0]).total_seconds() // 600))); w = (t - ts[k]).total_seconds() / 600
        return rows[k][1] * (1 - w) + rows[k + 1][1] * w, rows[k][2] * (1 - w) + rows[k + 1][2] * w, float(rows[k][3])
    s = interp('Saturn'); out = {}
    for n in eph:
        if n == 'Saturn': continue
        m = interp(n); out[n] = ((m[0] - s[0]) * np.cos(np.deg2rad(s[1])) * 3600, (m[1] - s[1]) * 3600, m[2])      # E, N arcsec, mag
    return out


# ---------------------------------------------------------------- stage 4: Iapetus (needs a much wider field than the crops)
NI = 80                                               # Iapetus cutout, half-res px
def _iap(args):
    s, u_off, v_off = args                            # window start; Iapetus offset from Saturn (half-res px, north-up: right/down)
    try:
        t = G['t']; q = G['q']; poly = G['poly']; order = np.argsort(-q[s:s + WIN]) + s; idx = order[:int(WIN * 0.6)]
        W = 440; acc = []
        for i in idx:
            d = fits.getdata(os.path.join(G['frames'], G['names'][i])).astype(np.float32) - BL; b = bin2(d); cy, cx = centroid(b)
            acc.append(northup(b, G['ori'][i] + corr_angle(t[i], poly), cx, cy, W))
        acc = np.array(acc); r0 = slice(W // 2 - 24, W // 2 + 24); win = np.hanning(48)[:, None] * np.hanning(48)[None, :]
        for k in range(1, len(acc)):
            acc[k] = ndi.shift(acc[k], np.clip(phase_cross_correlation(acc[0][r0, r0] * win, acc[k][r0, r0] * win, upsample_factor=20, normalization=None)[0], -3, 3), order=3)
        m = acc.mean(0); m = m - ndi.median_filter(m, size=15)
        u = int(round(W / 2 + u_off)); v = int(round(W / 2 + v_off)); h = NI // 2
        return s, m[v - h:v + h, u - h:u + h].astype(np.float32)
    except Exception:
        import traceback; return s, traceback.format_exc()

def stage_iapetus(a):
    out = os.path.join(a.work, 'iapetus.pkl')
    if os.path.exists(out): return
    res = pickle.load(open(os.path.join(a.work, 'windows.pkl'), 'rb')); eph = ephemeris(a, res)
    Z = np.load(os.path.join(a.work, 'crops.npz'))
    G.update(t=Z['t'], q=Z['q'], ori=Z['orient'], names=[str(x) for x in Z['names']], frames=a.frames, poly=np.load(os.path.join(a.work, 'calib_poly.npy')))
    jobs = []
    for w in res:
        p = predict(eph, datetime.fromisoformat(w['time']))['Iapetus']; jobs.append((w['s'], -p[0] / SH, -p[1] / SH))
    with Pool(10) as pl: cut = pl.map(_iap, jobs, chunksize=1)
    bad = [c for c in cut if isinstance(c[1], str)]
    if bad: print(bad[0][1]); sys.exit(1)
    pickle.dump(dict(cut=[c[1] for c in cut], starts=[c[0] for c in cut]), open(out, 'wb'))


# ---------------------------------------------------------------- stage 5: detection + video
def detect(w, eph):
    """Asymmetric S/N at predicted positions on the near-field stack: peak minus the point-reflected peak (Saturn glare is symmetric)."""
    Bm = w['field']; hp = Bm - ndi.median_filter(Bm, size=15); mf = cv2.GaussianBlur(hp, (0, 0), 1.4); mr = np.rot90(mf, 2)
    yy, xx = np.mgrid[:NB, :NB]; far = np.hypot(yy - NB / 2, xx - NB / 2) > 45; sg = 1.4826 * np.median(np.abs(mf[far] - np.median(mf[far])))
    det = {}
    for nm, (xi, eta, m) in predict(eph, datetime.fromisoformat(w['time'])).items():
        u = NB / 2 - xi / SH; v = NB / 2 - eta / SH
        if not (5 < u < NB - 5 and 5 < v < NB - 5): continue
        ui, vi = int(round(u)), int(round(v)); sl = (slice(vi - 2, vi + 3), slice(ui - 2, ui + 3))
        ann = (np.hypot(yy - vi, xx - ui) >= 6) & (np.hypot(yy - vi, xx - ui) <= 10)
        local = float((mf[sl].max() - np.median(mf[ann])) / sg)            # peak above its immediate surroundings, in far-field sigmas
        det[nm] = dict(u=u, v=v, snr=local, asym=float((mf[sl].max() - mr[sl].max()) / (sg * 1.414)), mag=m, r=float(np.hypot(xi, eta)))
    return hp, sg, det

def stage_render(a):
    res = pickle.load(open(os.path.join(a.work, 'windows.pkl'), 'rb')); eph = ephemeris(a, res)
    iap = pickle.load(open(os.path.join(a.work, 'iapetus.pkl'), 'rb')) if os.path.exists(os.path.join(a.work, 'iapetus.pkl')) else None
    os.makedirs(a.out, exist_ok=True); font = cv2.FONT_HERSHEY_SIMPLEX
    q0 = max(w['q'] for w in res)
    # one fixed stretch for every Saturn frame, set from the best window, so the brightness trend is honest
    def prep(img): c = img[HALF * S - 36 * S:HALF * S + 36 * S, HALF * S - 36 * S:HALF * S + 36 * S]; return np.clip(c, 0, None)
    scale = prep(res[int(np.argmax([w['q'] for w in res]))]['sat']).max()
    def sat8(img):
        c = prep(img); sharp = c + 0.7 * (c - cv2.GaussianBlur(c, (0, 0), 2.0))
        return (np.clip(np.clip(sharp, 0, None) / scale, 0, 1) ** 0.6 * 255).astype(np.uint8)
    W_, H_ = 1280, 720; vw = cv2.VideoWriter(os.path.join(a.work, 'raw.mp4'), cv2.VideoWriter_fourcc(*'mp4v'), 10, (W_, H_)); stills = {}
    for n, w in enumerate(res):
        hp, sg, det = detect(w, eph)
        A = cv2.cvtColor(cv2.resize(sat8(w['sat']), (480, 480), interpolation=cv2.INTER_CUBIC), cv2.COLOR_GRAY2BGR)
        cv2.line(A, (16, 462), (16 + int(20 / ARC * S * 480 / (72 * S)), 462), (255, 255, 255), 2); cv2.putText(A, '20"', (16, 452), font, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
        yy, xx = np.mgrid[:NB, :NB]; far = np.hypot(yy - NB / 2, xx - NB / 2) > 45; hs = cv2.GaussianBlur(hp, (0, 0), 1.0); sgd = 1.4826 * np.median(np.abs(hs[far] - np.median(hs[far])))
        x = (hs - np.median(hs[far])) / sgd; b = (np.clip(np.arcsinh(np.clip(x, 0, None) / 3) / np.arcsinh(10), 0, 1) * 255).astype(np.uint8)
        B = cv2.cvtColor(cv2.resize(b, (480, 480), interpolation=cv2.INTER_CUBIC), cv2.COLOR_GRAY2BGR); k = 480 / NB
        for nm, col in (('Titan', (80, 200, 255)), ('Rhea', (120, 255, 120))):
            if nm in det:
                d = det[nm]; ok = d['snr'] >= 5; p = (int(d['u'] * k), int(d['v'] * k)); c = col if ok else (110, 110, 110)
                cv2.circle(B, p, 16, c, 1, cv2.LINE_AA); cv2.putText(B, '%s%s' % (nm, '' if ok else ' ?'), (p[0] + 20, p[1] - 4), font, 0.45, c, 1, cv2.LINE_AA)
        L = int(60 / SH * k); cv2.line(B, (16, 462), (16 + L, 462), (255, 255, 255), 2); cv2.putText(B, '60"', (16, 452), font, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
        cv2.arrowedLine(B, (440, 50), (440, 18), (255, 255, 255), 1, tipLength=0.3); cv2.putText(B, 'N', (435, 14), font, 0.4, (255, 255, 255), 1)
        cv2.arrowedLine(B, (440, 50), (410, 50), (255, 255, 255), 1, tipLength=0.3); cv2.putText(B, 'E', (398, 54), font, 0.4, (255, 255, 255), 1)
        Cn = np.zeros((480, 320, 3), np.uint8)
        if iap and n < len(iap['cut']):
            cut = cv2.GaussianBlur(iap['cut'][n], (0, 0), 1.0); sgi = 1.4826 * np.median(np.abs(cut - np.median(cut))); xi_ = (cut - np.median(cut)) / sgi
            ci = (np.clip(np.arcsinh(np.clip(xi_, 0, None) / 3) / np.arcsinh(10), 0, 1) * 255).astype(np.uint8)
            Cn[80:400] = cv2.cvtColor(cv2.resize(ci, (320, 320), interpolation=cv2.INTER_CUBIC), cv2.COLOR_GRAY2BGR)
            cv2.circle(Cn, (160, 240), 22, (0, 200, 255), 1, cv2.LINE_AA); cv2.putText(Cn, 'IAPETUS (mag 11)', (30, 70), font, 0.5, (0, 200, 255), 1, cv2.LINE_AA)
            cv2.putText(Cn, '+-118" around predicted position', (14, 420), font, 0.4, (200, 200, 200), 1, cv2.LINE_AA)
        cv = np.zeros((H_, W_, 3), np.uint8); cv[80:560, 0:480] = A; cv[80:560, 480:960] = B; cv[80:560, 960:1280] = Cn
        cv2.putText(cv, 'Saturn: best %d of %d frames, 2x drizzle' % (w['nsat'], WIN), (10, 30), font, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
        cv2.putText(cv, 'UTC ' + w['time'][:19].replace('T', ' '), (10, 62), font, 0.55, (200, 200, 200), 1, cv2.LINE_AA)
        cv2.putText(cv, 'Near field +-189" (glare suppressed)', (490, 30), font, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
        cv2.putText(cv, 'IAPETUS (544\" from Saturn)', (970, 30), font, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
        # honest sharpness readout: focus drifted over the night
        frac = w['q'] / q0; cv2.putText(cv, 'Frame sharpness vs best window: %d%%' % round(100 * frac), (10, 600), font, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
        cv2.rectangle(cv, (10, 612), (470, 628), (90, 90, 90), 1); cv2.rectangle(cv, (10, 612), (10 + int(460 * frac), 628), (80, 200, 255), -1)
        for j, (nm, d) in enumerate(sorted(det.items(), key=lambda kv: kv[1]['r'])):
            if nm in ('Titan', 'Rhea'): cv2.putText(cv, '%s mag %.1f  %.0f"  S/N %.0f' % (nm, d['mag'], d['r'], d['snr']), (490, 600 + 22 * (j % 3)), font, 0.45, (200, 200, 200), 1, cv2.LINE_AA)
        cv2.putText(cv, 'Inner moons (Enceladus, Dione, Tethys, Mimas) lie inside the glare: not identifiable.', (10, 690), font, 0.45, (160, 160, 160), 1, cv2.LINE_AA)
        vw.write(cv)
        if n == int(np.argmax([x['q'] for x in res])): stills['best'] = cv.copy()
        if n == len(res) - 1: stills['last'] = cv.copy()
    vw.release()
    final = os.path.join(a.out, 'saturn_moons_v2.mp4')
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', os.path.join(a.work, 'raw.mp4'), '-c:v', 'libopenh264', '-b:v', '5M', '-pix_fmt', 'yuv420p', final], check=True)
    for k, v in stills.items(): cv2.imwrite(os.path.join(a.out, f'saturn_moons_v2_{k}.png'), v)
    print(len(res), 'frames ->', final)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames', default=os.path.expanduser('~/Pictures/CelestronOrigin')); ap.add_argument('--work', default='build/saturn_moons')
    ap.add_argument('--out', default=os.path.expanduser('~/Pictures/CelestronStacks')); ap.add_argument('--stage', default='all')
    ap.add_argument('--no-iapetus', action='store_true')
    a = ap.parse_args(); os.makedirs(a.work, exist_ok=True)
    stages = [('crops', stage_crops), ('calib', stage_calib), ('windows', stage_windows), ('iapetus', stage_iapetus), ('render', stage_render)]
    for name, fn in stages:
        if a.stage in ('all', name) and not (name == 'iapetus' and a.no_iapetus):
            print('== stage', name, flush=True)
            if name in ('iapetus', 'render'):
                pass
            fn(a)

if __name__ == '__main__':
    main()
