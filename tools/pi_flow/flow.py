#!/usr/bin/env python3
"""Data-driven processing of a live-stack result with the installed PixInsight run headless (prototype).

    flow.py --stack STACK.fits [--swap-rb] --ref-header RAW_FRAME.fits --target "M 1" --nframes 142 --total-s 2438 --out DIR
    flow.py --frames FRAMES.txt --target "M 31" --out DIR           # builds the stack first: Debayer (GRBG), StarAlignment, ImageIntegration

Stages: measure the data conditions -> classify the target -> choose steps and parameters (recipe.py, each with a reason) -> astrometry against Gaia DR3 -> SpectrophotometricColorCalibration ->
deconvolution with the iteration count searched against sharpening, ringing and noise -> TGV denoise -> colour-preserving render -> report. PixInsight is driven only through its scripting interface;
no PixInsight Class Library source is used or derived here.
"""
import argparse, csv, json, math, os, re, subprocess, sys, time
import numpy as np
from astropy.io import fits
from scipy import ndimage as ndi
from scipy.spatial import cKDTree
from PIL import Image
import assess, recipe, pi_run


def log(msg): print('[%s] %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


# ------------------------------------------------------------------------------------------------ stack from frames
def stack_from_frames(frames, work, rejection, ref_index=None):
    """raw Bayer FITS -> debayer (the Origin's layout is GRBG as PixInsight reads the file) -> align -> integrate. Returns the path of the integrated FITS with red and blue in their right order."""
    raw = os.path.join(work, 'raw')
    for sub in ('raw', 'deb', 'reg'): os.makedirs(os.path.join(work, sub), exist_ok=True)
    for i, f in enumerate(frames):
        dst = os.path.join(raw, 'f_%03d.fits' % i)
        if not os.path.exists(dst): os.symlink(os.path.abspath(f), dst)
    ref = len(frames) // 2 if ref_index is None else ref_index
    rejmap = {'PercentileClip': 'PercentileClip', 'SigmaClip': 'SigmaClip', 'WinsorizedSigmaClip': 'WinsorizedSigmaClip', 'LinearFit': 'LinearFit'}
    body = '''
var base = "%(w)s/";
var files = searchDirectory(base + "raw/*.fits", false).sort();
var D = new Debayer; D.bayerPattern = Debayer.prototype.GBRG; D.debayerMethod = Debayer.prototype.VNG; D.outputDirectory = base + "deb"; D.outputPostfix = "_d"; D.overwriteExistingFiles = true;
D.targetItems = files.map(function(f){ return [true, f]; }); D.executeGlobal(); __out += "debayered\\n";
var deb = searchDirectory(base + "deb/*.xisf", false).sort();
var A = new StarAlignment; A.referenceImage = deb[%(ref)d]; A.referenceIsFile = true; A.targets = deb.map(function(f){ return [true, true, f]; });
A.outputDirectory = base + "reg"; A.overwriteExistingFiles = true; A.onError = StarAlignment.prototype.Continue; A.generateMasks = false; A.executeGlobal(); __out += "aligned\\n";
var reg = searchDirectory(base + "reg/*.xisf", false).sort();
var I = new ImageIntegration; I.images = reg.map(function(f){ return [true, f, "", ""]; }); I.combination = ImageIntegration.prototype.Average;
I.rejection = ImageIntegration.prototype.%(rej)s; I.normalization = ImageIntegration.prototype.AdditiveWithScaling; I.weightMode = ImageIntegration.prototype.NoiseEvaluation;
I.rejectionNormalization = ImageIntegration.prototype.Scale; I.sigmaLow = 4; I.sigmaHigh = 3; I.generateRejectionMaps = false; I.generateIntegratedImage = true; I.evaluateNoise = true;
I.executeGlobal(); var w = ImageWindow.activeWindow; w.saveAs(base + "integrated.fits", false, false, false, false); __out += "integrated " + reg.length + " frames\\n";
''' % {'w': work, 'ref': ref, 'rej': rejmap[rejection]}
    ok, text, dt = pi_run.run(body, work, 'stack', timeout=7200)
    log('stack from %d frames: %s in %.0f s' % (len(frames), 'ok' if ok else 'FAILED', dt))
    if not ok: raise RuntimeError('stack failed:\n' + text[-1500:])
    res, good = check_registration(work, len(frames))
    if not good: raise RuntimeError('StarAlignment left the frames unregistered (%s): do not use this stack; use the live stacker registration instead' % res)
    return os.path.join(work, 'integrated.fits')


def check_registration(work, n, min_fraction=0.7):
    """StarAlignment can report success and still leave frames essentially unregistered (seen 2026-10-14: 180 frames of 4 s at altitude 81, field rotating fast, "180 succeeded" and a stack of star trails).
    Export a few registered frames and compare their brightest stars with the reference frame's: the fraction that coincide within 2 px must be high. Returns (fractions, ok)."""
    ids = sorted({0, n // 4, n // 2, (3 * n) // 4, n - 1}); body = ('var ids = %s; for (var i = 0; i < ids.length; ++i) { var s = ("000" + ids[i]).slice(-3); var w = ImageWindow.open("%s/reg/f_" + s + "_d_r.xisf")[0];'
                                                                     ' w.saveAs("%s/chk_" + s + ".fits", false, false, false, false); w.forceClose(); }') % (ids, work, work)
    ok, text, dt = pi_run.run(body, work, 'chk')
    def peaks(p):
        d = fits.getdata(p).astype(np.float64); L = d.mean(0); H, W = L.shape; Ls = ndi.gaussian_filter(L, 1.0); bg = ndi.zoom(ndi.median_filter(L[::4, ::4], size=25), 4, order=1)[:H, :W]
        valid = ndi.binary_erosion(L > 0, iterations=20); pk = (Ls == ndi.maximum_filter(Ls, size=9)) & valid & (Ls - bg > 0.03); y, x = np.nonzero(pk); a = (Ls - bg)[y, x]; o = np.argsort(-a)[:300]
        return np.c_[x[o], y[o]].astype(float)
    ref = cKDTree(peaks(os.path.join(work, 'chk_%03d.fits' % (n // 2)))); res = {}
    for i in ids:
        if i == n // 2: continue
        dist, _ = ref.query(peaks(os.path.join(work, 'chk_%03d.fits' % i))); res[i] = float(np.mean(dist < 2))
    good = sum(1 for v in res.values() if v >= min_fraction) >= max(1, len(res) - 1)
    log('registration check: fraction of bright stars within 2 px of the reference %s -> %s' % ({k: round(v, 2) for k, v in res.items()}, 'ok' if good else 'FAILED'))
    return res, good


# ------------------------------------------------------------------------------------------------ astrometry (Gaia DR3 through the ESA TAP service)
def gaia_cone(ra, dec, radius, work, gmax=18.5):
    """Gaia DR3 stars in a cone, from the local PixInsight database through its Gaia process (about 0.1 s, offline, complete to G=16.6, which is deeper than the matching uses). The ESA TAP service is
    only the fallback; it times out on dense fields."""
    path = os.path.join(work, 'gaia_local.csv')
    body = ('var P = new Gaia; P.dataRelease = Gaia.prototype.DataRelease_3; P.centerRA = %.6f; P.centerDec = %.6f; P.radius = %.4f; P.magnitudeLow = -1.5; P.magnitudeHigh = %.2f; P.sortBy = Gaia.prototype.SortBy_G;'
            ' P.generateTextOutput = true; P.textFormat = Gaia.prototype.TextFormat_CSV; P.textHeaders = Gaia.prototype.TextHeaders_TableColumns; P.outputFilePath = "%s"; var ok = P.executeGlobal(); __out += "gaia " + ok + " " + P.sources.length + "\\n";') % (ra, dec, radius, gmax, path)
    if os.path.exists(path): os.remove(path)
    ok, text, dt = pi_run.run(body, work, 'gaia')
    if ok and os.path.exists(path) and open(path).readline().startswith('ra,'):
        g = np.genfromtxt(path, delimiter=',', names=True); log('Gaia DR3 (local PixInsight database): %d stars in %.0f s, G %.1f to %.1f' % (len(g), dt, g['phot_g_mean_mag'].min(), g['phot_g_mean_mag'].max())); return g
    log('local Gaia query failed; falling back to the ESA TAP service')
    path = os.path.join(work, 'gaia.csv')
    for gl in (17.0, 16.0, 15.0):
        q = ("SELECT TOP 40000 source_id,ra,dec,phot_g_mean_mag,bp_rp FROM gaiadr3.gaia_source WHERE 1=CONTAINS(POINT('ICRS',ra,dec),CIRCLE('ICRS',%.5f,%.5f,%.3f)) AND phot_g_mean_mag<%.1f AND bp_rp IS NOT NULL" % (ra, dec, radius, gl))
        subprocess.run(['curl', '-s', '-m', '240', '--data-urlencode', 'QUERY=' + q, '-d', 'REQUEST=doQuery&LANG=ADQL&FORMAT=csv', 'https://gea.esac.esa.int/tap-server/tap/sync', '-o', path])
        if os.path.exists(path) and open(path).readline().startswith('source_id'): return np.genfromtxt(path, delimiter=',', names=True)
    raise RuntimeError('no Gaia source available')


def solve_wcs(L, ref_hdr, work):
    """plate-match the stack to Gaia: start from the pointing and orientation in the raw frame header, find the offset by voting, refine an affine transform. Returns a WCS header (FITS convention, y up) or None."""
    H, W = L.shape; ra0, dec0 = ref_hdr['CRVAL1'], ref_hdr['CRVAL2']
    g = gaia_cone(ra0, dec0, 0.7, work)
    Ls = ndi.gaussian_filter(L, 1.0); bg = ndi.zoom(ndi.median_filter(L[::4, ::4], size=25), 4, order=1)[:H, :W]
    valid = ndi.binary_erosion(L > 0, iterations=30)
    pk = (Ls == ndi.maximum_filter(Ls, size=9)) & valid & (Ls - bg > 0.008); py, px = np.nonzero(pk)
    ra, dec = np.radians(g['ra']), np.radians(g['dec']); r0, d0 = np.radians(ra0), np.radians(dec0)
    cosc = np.sin(d0) * np.sin(dec) + np.cos(d0) * np.cos(dec) * np.cos(ra - r0)
    xi = np.degrees(np.cos(dec) * np.sin(ra - r0) / cosc); eta = np.degrees((np.cos(d0) * np.sin(dec) - np.sin(d0) * np.cos(dec) * np.cos(ra - r0)) / cosc)
    s1, s2, rot0 = ref_hdr['CDELT1'], ref_hdr['CDELT2'], math.radians(ref_hdr['CROTA2'])
    amp = (Ls - bg)[py, px]; top = np.argsort(-amp)[:1500]; ppx, ppy = px[top].astype(float), py[top].astype(float); bright = np.argsort(g['phot_g_mean_mag'])[:400]
    def cd_for(delta_deg):
        rot = rot0 + math.radians(delta_deg); return np.array([[s1 * math.cos(rot), -s2 * math.sin(rot)], [s1 * math.sin(rot), s2 * math.cos(rot)]])
    def vote(delta_deg, flips, search, bin_px, nbright, center=(0.0, 0.0)):
        edges = np.arange(-search, search + bin_px, bin_px); uv = (np.linalg.inv(cd_for(delta_deg)) @ np.vstack([xi, eta])).T; out = None
        for fx, fy in flips:
            X = W / 2 + uv[bright[:nbright], 0] * (-1 if fx else 1); Y = H / 2 + uv[bright[:nbright], 1] * (-1 if fy else 1)
            dx = (ppx[None, :] - X[:, None]).ravel() - center[0]; dy = (ppy[None, :] - Y[:, None]).ravel() - center[1]; m = (np.abs(dx) < search) & (np.abs(dy) < search)
            hist, xe, ye = np.histogram2d(dx[m], dy[m], bins=[edges, edges]); k = np.unravel_index(hist.argmax(), hist.shape); mean = float(hist.mean()) + 1e-9
            z = (hist.max() - mean) / math.sqrt(mean)
            if out is None or z > out[0]: out = (z, fx, fy, xe[k[0]] + bin_px / 2 + center[0], ye[k[1]] + bin_px / 2 + center[1], mean, delta_deg, hist.max())
        return out
    # coarse: orientation within +-12 deg of the header (then the full circle with flips if that fails), offsets within +-0.74 deg; fine: 3 px bins around the coarse peak
    best = None
    for pass_deltas, flips in ((np.arange(-12, 12.01, 0.5), [(False, False)]), (np.arange(-180, 180, 1.0), [(False, False), (True, False), (False, True), (True, True)])):
        for dd in pass_deltas:
            r = vote(dd, flips, 1800.0, 20.0, 250)
            if r and (best is None or r[0] > best[0]): best = r
        if best and best[0] >= 8: break
    if best is None or best[0] < 8: log('astrometry: no convincing offset peak (best z %.1f)' % (best[0] if best else float('nan'))); return None
    fine = None
    for dd in np.arange(best[6] - 0.6, best[6] + 0.61, 0.1):
        r = vote(dd, [(best[1], best[2])], 45.0, 3.0, 400, center=(best[3], best[4]))
        if r and (fine is None or r[0] > fine[0]): fine = r
    best = fine if fine and fine[0] >= best[0] * 0.5 else best
    log('astrometry: header orientation corrected by %.1f deg, offset (%.0f, %.0f) px = %.2f deg, vote %d (z %.1f)' % (best[6], best[3], best[4], math.hypot(best[3], best[4]) * assess.ARCSEC_PER_PX / 3600, best[7], best[0]))
    CD = cd_for(best[6]); fx, fy, tx, ty = best[1], best[2], best[3], best[4]; uv = (np.linalg.inv(CD) @ np.vstack([xi, eta])).T
    u = uv[:, 0] * (-1 if fx else 1); v = uv[:, 1] * (-1 if fy else 1); P = np.c_[W / 2 + u + tx, H / 2 + v + ty, np.ones(len(u))]; A = np.eye(3)[:2]; sel = g['phot_g_mean_mag'] < 17.0
    det = np.c_[px, py].astype(float); tree = cKDTree(det); n = 0; rms = float('nan')
    for rad in (25, 15, 8, 5, 3, 2.5):
        pred = P @ A.T; dist, idx = tree.query(pred[sel]); ok = dist < rad; ii = np.nonzero(sel)[0][ok]; order = np.argsort(dist[ok]); seen = set(); keep = []
        for o in order:
            j = idx[ok][o]
            if j in seen: continue
            seen.add(j); keep.append(o)
        ii = ii[keep]; jj = idx[ok][keep]
        if len(ii) < 12: log('astrometry: only %d matches at %.0f px' % (len(ii), rad)); return None
        A = np.linalg.lstsq(P[ii], det[jj], rcond=None)[0].T; res = np.hypot(*(P[ii] @ A.T - det[jj]).T); n = len(ii); rms = float(np.sqrt((res ** 2).mean()))
    log('astrometry: %d Gaia stars matched, rms %.2f px (flip x %s y %s)' % (n, rms, fx, fy))
    if rms > 1.5 or n < 100: return None
    M = np.linalg.inv(A[:, :2]); t0 = -M @ A[:, 2]; c = np.array([W / 2 + tx, H / 2 + ty]); CDp = CD @ M; pref = np.linalg.solve(M, c - t0)
    hd = {'CTYPE1': 'RA---TAN', 'CTYPE2': 'DEC--TAN', 'CRVAL1': ra0, 'CRVAL2': dec0, 'CRPIX1': float(pref[0] + 1), 'CRPIX2': float(pref[1] + 1), 'CD1_1': float(CDp[0, 0]), 'CD1_2': float(CDp[0, 1]),
          'CD2_1': float(CDp[1, 0]), 'CD2_2': float(CDp[1, 1]), 'RADESYS': 'ICRS', 'EQUINOX': 2000.0}
    hd['_matches'] = n; hd['_rms_px'] = rms
    return hd


def world2pix(hd, ra, dec):
    """(x, y) 0-based, y up, of a sky position under the linear WCS above"""
    r0, d0 = math.radians(hd['CRVAL1']), math.radians(hd['CRVAL2']); r, d = math.radians(ra), math.radians(dec)
    cosc = math.sin(d0) * math.sin(d) + math.cos(d0) * math.cos(d) * math.cos(r - r0)
    xi = math.degrees(math.cos(d) * math.sin(r - r0) / cosc); eta = math.degrees((math.cos(d0) * math.sin(d) - math.sin(d0) * math.cos(d) * math.cos(r - r0)) / cosc)
    CD = np.array([[hd['CD1_1'], hd['CD1_2']], [hd['CD2_1'], hd['CD2_2']]]); o = np.linalg.solve(CD, [xi, eta])
    return o[0] + hd['CRPIX1'] - 1, o[1] + hd['CRPIX2'] - 1


# ------------------------------------------------------------------------------------------------ PixInsight steps
def spcc_gains(d_bg, wcs, ref_hdr, work):
    """SpectrophotometricColorCalibration on the background-subtracted stack. The FITS is written vertically mirrored with a matching WCS: PixInsight places the solution for this file layout that way
    (otherwise every catalogue star mismatches). Returns the white-balance factors (R, G, B) or None."""
    C, H, W = d_bg.shape; hd = fits.Header()
    for k, v in wcs.items():
        if not k.startswith('_'): hd[k] = v
    hd['CRPIX2'] = H + 1 - wcs['CRPIX2']; hd['CD1_2'] = -wcs['CD1_2']; hd['CD2_2'] = -wcs['CD2_2']
    hd['DATE-OBS'] = ref_hdr.get('DATE-OBS', '2026-01-01T00:00:00'); hd['EXPTIME'] = 1.0; hd['LONG-OBS'] = ref_hdr.get('LONGITUD', 0.0797); hd['LAT-OBS'] = ref_hdr.get('LATITUDE', 52.245)
    hd['XPIXSZ'] = 2.4; hd['YPIXSZ'] = 2.4; hd['FOCALLEN'] = 335.0
    path = os.path.join(work, 'spcc_in.fits'); fits.PrimaryHDU(np.clip(d_bg, 0, 1)[:, ::-1, :].astype(np.float32), header=hd).writeto(path, overwrite=True)
    body = 'var w = ImageWindow.open("%s")[0]; var P = new SpectrophotometricColorCalibration; P.generateGraphs = false; P.generateTextFiles = false; P.generateStarMaps = false; __out += "executed " + P.executeOn(w.mainView) + "\\n";' % path
    ok, text, dt = pi_run.run(body, work, 'spcc')
    m = {k: re.search(r'W_%s : ([0-9.]+)' % k, text) for k in 'RGB'}
    if not all(m.values()): log('SPCC failed: ' + (re.findall(r'\*\*\* Error: .*', text) or [text[-300:]])[0]); return None
    w = [float(m[k].group(1)) for k in 'RGB']; log('SPCC: white-balance factors R %.4f G %.4f B %.4f (%.0f s)' % (*w, dt)); return w


def deconvolve_variants(src_fits, psf, iterations, work):
    """run Richardson-Lucy (regularized, luminance only, with deringing) for each iteration count; returns {n: path}"""
    pp = os.path.join(work, 'psf.fits'); fits.PrimaryHDU(psf.astype(np.float32)).writeto(pp, overwrite=True)
    body = '''var pw = ImageWindow.open("%(psf)s")[0]; pw.mainView.id = "psf"; var its = %(its)s;
for (var i = 0; i < its.length; ++i) { var w = ImageWindow.open("%(src)s")[0]; var P = new Deconvolution; P.algorithm = Deconvolution.prototype.RichardsonLucy; P.numberOfIterations = its[i];
 P.psfMode = Deconvolution.prototype.External; P.psfViewId = "psf"; P.deringing = true; P.deringingDark = 0.03; P.deringingBright = 0.0; P.useRegularization = true; P.toLuminance = true; P.useLuminance = true;
 var ok = P.executeOn(w.mainView); w.saveAs("%(w)s/decon_" + its[i] + ".fits", false, false, false, false); w.forceClose(); __out += "decon " + its[i] + " " + ok + "\\n"; }''' % {'psf': pp, 'its': json.dumps(iterations), 'src': src_fits, 'w': work}
    ok, text, dt = pi_run.run(body, work, 'decon'); log('deconvolution variants %s: %s (%.0f s)' % (iterations, 'ok' if ok else 'FAILED', dt))
    return {n: os.path.join(work, 'decon_%d.fits' % n) for n in iterations if os.path.exists(os.path.join(work, 'decon_%d.fits' % n))}


def repair_clipped(orig, decon):
    """Richardson-Lucy leaves black holes around clipped star cores: blend the original back over damaged or clipped regions"""
    Lo, Ld = orig.mean(0), decon.mean(0)
    bad = ((Ld < 0.5 * Lo) & (Lo > 0.03)) | (orig.max(0) > 0.85)
    grow = ndi.binary_dilation(bad, iterations=6); w = np.clip(ndi.gaussian_filter(grow.astype(float), 3.0) * 1.5, 0, 1)
    return decon * (1 - w) + orig * w, int(bad.sum())


def tgv_search(src_fits, noise, valid, den, work):
    """TGV denoise at each strength with the edge protection tied to the measured noise; keep the smallest strength that reaches the target noise ratio (or the strongest if none does)"""
    its = den['strengths_to_try']; edge = den['edge_over_noise'] * noise
    body = ('var s = %s; for (var i = 0; i < s.length; ++i) { var w = ImageWindow.open("%s")[0]; var P = new TGVDenoise; P.strengthL = s[i]; P.strengthC = s[i] * 2; P.edgeProtectionL = %g; P.edgeProtectionC = %g;'
            ' var ok = P.executeOn(w.mainView); w.saveAs("%s/tgv_" + i + ".fits", false, false, false, false); w.forceClose(); __out += "tgv " + i + " " + ok + "\\n"; }') % (json.dumps(its), src_fits, edge, 1.5 * edge, work)
    ok, text, dt = pi_run.run(body, work, 'tgv'); log('TGV variants %s edge %.5f: %s (%.0f s)' % (its, edge, 'ok' if ok else 'FAILED', dt))
    src = fits.getdata(src_fits).astype(np.float64); n0 = assess.hp_noise(src.mean(0), valid); rows = []; chosen = None
    for i, s in enumerate(its):
        p = os.path.join(work, 'tgv_%d.fits' % i)
        if not os.path.exists(p): continue
        x = fits.getdata(p).astype(np.float64); ratio = assess.hp_noise(x.mean(0), valid) / n0; rows.append({'strength': s, 'noise_ratio': ratio, 'core_fwhm_px': assess.core_fwhm(x, valid)[0]})
        log('  TGV strength %.1f: noise x%.2f' % (s, ratio))
        if chosen is None and ratio <= den['target_noise_ratio']: chosen = (s, x, ratio)
    if chosen is None and rows: i = len(rows) - 1; chosen = (rows[i]['strength'], fits.getdata(os.path.join(work, 'tgv_%d.fits' % i)).astype(np.float64), rows[i]['noise_ratio'])
    return chosen, rows


# ------------------------------------------------------------------------------------------------ render
def render(d, valid, gains, rec, center_xy, crop_px, out_png, fade_clipped=True, orig_for_clip=None):
    """colour-preserving arcsinh render of the (calibrated, background-flattened) linear stack, sky-correct orientation (FITS row 0 is the bottom), cropped around the target"""
    C, H, W = d.shape
    x = np.clip(d * (np.array(gains) / np.mean(gains))[:, None, None], 0, None); x[:, ~valid] = 0; L = x.mean(0)
    sky = float(np.median(L[valid])); noise = assess.hp_noise(L, valid); black = sky - rec['stretch']['black_sigma'] * noise; Lb = np.clip(L - black, 0, None)
    sm = ndi.gaussian_filter(Lb, 3.0)
    if rec['stretch']['reference'] == 'target region':
        yy, xx = np.mgrid[0:H, 0:W]; disk = (np.hypot(xx - center_xy[0], yy - center_xy[1]) < crop_px / 8) & valid; ref = float(np.median(sm[disk]))
    else: ref = float(np.percentile(sm[valid], 70))
    ref = max(ref, 3 * noise); lvl = rec['stretch']['target_display_level']; xmax = float(Lb.max())
    lo, hi = 1e-5, 10.0
    for _ in range(60):                                         # solve asinh(ref/s)/asinh(xmax/s) = level for s (bisection on a monotone function)
        s = math.sqrt(lo * hi)
        if math.asinh(ref / s) / math.asinh(xmax / s) < lvl: hi = s
        else: lo = s
    s = math.sqrt(lo * hi); Ls = np.arcsinh(Lb / s) / math.asinh(xmax / s)
    xs_ = np.stack([ndi.gaussian_filter(x[c], 2.0) for c in range(3)]); Lm = np.clip(xs_.mean(0), 1e-6, None); ratio = xs_ / Lm
    rgb = np.clip(1 + (ratio - 1) * rec['saturation'], 0, 3); fade = np.clip((ndi.gaussian_filter(L, 2.0) - 0.5 * noise) / (3 * noise), 0, 1); rgb = 1 + (rgb - 1) * fade
    if fade_clipped:
        src = orig_for_clip if orig_for_clip is not None else d
        cw = ndi.gaussian_filter(ndi.binary_dilation(src.max(0) > 0.85, iterations=3).astype(float), 1.5); rgb = 1 + (rgb - 1) * (1 - cw)
    out = rgb * Ls
    # local contrast on the stretched luminance (unsharp at a large scale), applied as a ratio so colour is kept
    a = rec['clarity']
    if a > 0:
        lum = out.mean(0); loc = lum + a * (lum - ndi.gaussian_filter(lum, 25)); out = out * (np.clip(loc, 1e-4, None) / np.clip(lum, 1e-4, None))
    out = np.clip(out, 0, 1); out[:, ~valid] = 0
    img = (out.transpose(1, 2, 0)[::-1] * 255 + 0.5).astype(np.uint8)
    cx, cy = center_xy; half = crop_px // 2; cyf = H - 1 - cy
    crop = img[max(0, int(cyf) - half * 3 // 4):int(cyf) + half * 3 // 4, max(0, int(cx) - half):int(cx) + half]
    Image.fromarray(crop).resize((crop.shape[1] * 2, crop.shape[0] * 2), Image.LANCZOS).save(out_png)
    return {'asinh_scale': s, 'reference_level': ref, 'sky': sky, 'noise': noise}


# ------------------------------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stack'); ap.add_argument('--swap-rb', action='store_true'); ap.add_argument('--frames'); ap.add_argument('--ref-header', help='a raw frame of the target (pointing and orientation for the astrometry)')
    ap.add_argument('--target', required=True); ap.add_argument('--nframes', type=int); ap.add_argument('--total-s', type=float); ap.add_argument('--out', required=True)
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True); work = os.path.join(a.out, 'work'); os.makedirs(work, exist_ok=True); T = {'start': time.time()}
    rep = {'target': a.target}
    cls, size_arcmin, faint = assess.classify(a.target); rep['class'] = {'class': cls, 'size_arcmin': size_arcmin, 'faintness': faint}; log('target %s: %s, %.0f arcmin, faintness %.2f' % (a.target, cls, size_arcmin, faint))
    frames = None
    if a.frames:
        frames = [l.strip() for l in open(a.frames) if l.strip()]; a.nframes = len(frames); a.ref_header = a.ref_header or frames[len(frames) // 2]
        a.total_s = a.total_s or sum(float(fits.getheader(f).get('EXPTIME', 0)) for f in frames)
        a.stack = stack_from_frames(frames, work, recipe.choose({'gradient_rms_over_noise': 0, 'psf_fwhm_px': float('nan'), 'psf_ellipticity': 0, 'psf_stars': 0, 'star_density_per_mpx': 0, 'saturated_star_fraction': 0}, cls, size_arcmin, faint, float('nan'), a.nframes, a.total_s or 0)['steps']['integration']['rejection'])
        a.swap_rb = True
    ref_hdr = fits.getheader(a.ref_header); rep['frames'] = {'count': a.nframes, 'total_exposure_s': a.total_s}
    d, hdr = assess.load_stack(a.stack, a.swap_rb); valid = assess.valid_mask(d); C, H, W = d.shape
    # 1. measure
    t = time.time(); m, psf = assess.measure(d, valid); rep['metrics'] = m; log('measured: PSF FWHM %.2f px (ellipticity %.3f, %d stars), noise %.4f, gradient %.2f, %.0f stars/Mpx' % (m['psf_fwhm_px'], m['psf_ellipticity'], m['psf_stars'], m['noise_sigma'], m['gradient_rms_over_noise'], m['star_density_per_mpx']))
    # 2. astrometry and the target's position
    wcs = solve_wcs(d.mean(0), ref_hdr, work); rep['astrometry'] = None if wcs is None else {'matches': wcs['_matches'], 'rms_px': wcs['_rms_px']}
    if wcs is None: center = (W / 2, H / 2)
    else: center = world2pix(wcs, ref_hdr['CRVAL1'], ref_hdr['CRVAL2'])
    # 3. background and the target's own signal-to-noise
    pre = recipe.choose(m, cls, size_arcmin, faint, float('nan'), a.nframes or 0, a.total_s or 0); deg = pre['steps']['background']['degree']
    bg, _ = assess.fit_background(d, valid, deg); d_bg = d - bg; d_bg += 0.01 - np.median(d_bg[:, valid], axis=1)[:, None, None]; d_bg[:, ~valid] = 0
    radius = min(max(20, (size_arcmin * 60 / assess.ARCSEC_PER_PX) / 2 * 0.5), 0.18 * H); snr, sig = assess.object_snr(d_bg, center, radius, valid); rep['target_snr'] = {'snr': snr, 'signal': sig, 'radius_px': radius}
    log('background degree %d; target region SNR %.1f' % (deg, snr))
    # 4. the recipe
    plan = recipe.choose(m, cls, size_arcmin, faint, snr, a.nframes or 0, a.total_s or 0); rep['plan'] = plan
    for k, v in plan['why'].items(): log('  %-14s %s' % (k, v))
    for n in plan['notes']: log('  note: ' + n)
    bgpath = os.path.join(work, 'bgsub.fits'); fits.PrimaryHDU(np.clip(d_bg, 0, 1).astype(np.float32)).writeto(bgpath, overwrite=True)
    # 5. colour calibration
    gains = spcc_gains(d_bg, wcs, ref_hdr, work) if wcs is not None else None
    if gains is None:
        # fallback: neutral median star from aperture photometry would go here; use the sky-neutral background and a unity white balance
        gains = [1.0, 1.0, 1.0]; rep['colour'] = 'no calibration (astrometry or SPCC failed)'
    else: rep['colour'] = {'spcc_factors_RGB': gains}
    # 6. deconvolution with the iteration count searched
    final = d_bg; decon_report = None; dec = plan['steps']['deconvolution']
    if dec and psf is not None:
        its = dec['iterations_to_try']; paths = deconvolve_variants(bgpath, psf, its, work); base_fw, base_wing, _ = assess.core_fwhm(d_bg, valid); base_noise = assess.hp_noise(d_bg.mean(0), valid)
        rows = [{'iterations': 0, 'core_fwhm_px': base_fw, 'wing_min': base_wing, 'noise_ratio': 1.0, 'score': 0.0}]; best = None
        for n, p in sorted(paths.items()):
            v = fits.getdata(p).astype(np.float64); v, nrep = repair_clipped(d_bg, v); fw, wing, _ = assess.core_fwhm(v, valid); nz = assess.hp_noise(v.mean(0), valid) / base_noise
            ring = -wing / 0.001                                  # 1.0 = dark wings at 0.1% of the star's peak
            minfw = dec['min_core_fwhm_px']
            score = (base_fw - fw) / base_fw - 0.25 * max(0.0, ring - 0.5) - 3 * max(0.0, nz - 1.3) - 1.5 * max(0.0, minfw - fw) / minfw
            rows.append({'iterations': n, 'core_fwhm_px': fw, 'wing_min': wing, 'noise_ratio': nz, 'score': score, 'repaired_px': nrep})
            if best is None or score > best[0]: best = (score, n, v)
            log('  decon %3d it: core FWHM %.2f px (was %.2f), dark wings %.2f, noise x%.2f, score %.3f' % (n, fw, base_fw, ring, nz, score))
        decon_report = {'candidates': rows}
        if best and best[0] > 0.02: final = best[2]; decon_report['chosen_iterations'] = best[1]; log('deconvolution: chose %d iterations' % best[1])
        else: decon_report['chosen_iterations'] = 0; log('deconvolution: no candidate beat the unprocessed stack by the margin; skipped')
    rep['deconvolution'] = decon_report
    # 7. denoise
    dn = plan['steps']['denoise']; inp = os.path.join(work, 'pre_tgv.fits'); fits.PrimaryHDU(np.clip(final, 0, 1).astype(np.float32)).writeto(inp, overwrite=True)
    chosen, rows = tgv_search(inp, assess.hp_noise(final.mean(0), valid), valid, dn, work)
    if chosen: final_dn = chosen[1]; rep['denoise'] = {'candidates': rows, 'chosen_strength': chosen[0], 'noise_ratio': chosen[2]}; log('denoise: strength %.1f, noise x%.2f' % (chosen[0], chosen[2]))
    else: final_dn = final; rep['denoise'] = None
    # 8. render: baseline (no deconvolution or denoise) and processed, at the same recipe
    crop_px = int(min(max(size_arcmin * 60 / assess.ARCSEC_PER_PX * 3, 600), 1800))
    r0 = render(d_bg, valid, gains, plan['steps'], center, crop_px, os.path.join(a.out, 'baseline.png'), orig_for_clip=d_bg)
    r1 = render(final_dn, valid, gains, plan['steps'], center, crop_px, os.path.join(a.out, 'processed.png'), orig_for_clip=d_bg); rep['render'] = {'baseline': r0, 'processed': r1}
    rep['elapsed_s'] = time.time() - T['start']
    json.dump(rep, open(os.path.join(a.out, 'report.json'), 'w'), indent=1, default=float); log('done in %.0f s: %s' % (rep['elapsed_s'], a.out))


if __name__ == '__main__':
    main()
