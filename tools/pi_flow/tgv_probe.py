import sys, json, numpy as np
sys.path.insert(0, '.')
import assess, pi_run
from astropy.io import fits
from scipy import ndimage as ndi
W = '/mnt/fits-backup/claude/archive_qc/pi_flow/m1/work/'
grid = [(1.5, 0.002), (1.0, 0.0005), (0.5, 0.0005), (0.3, 0.0003), (0.2, 0.0003), (0.1, 0.0005), (0.5, 0.0001), (0.05, 0.0003)]
body = 'var g = %s; for (var i = 0; i < g.length; ++i) { var w = ImageWindow.open("%sdecon_20.fits")[0]; var P = new TGVDenoise; P.strengthL = g[i][0]; P.strengthC = g[i][0] * 2; P.edgeProtectionL = g[i][1]; P.edgeProtectionC = g[i][1] * 1.5;' \
       ' var ok = P.executeOn(w.mainView); w.saveAs("%stgv_" + i + ".fits", false, false, false, false); w.forceClose(); __out += "tgv " + i + " " + ok + "\\n"; }' % (json.dumps(grid), W, W)
ok, text, dt = pi_run.run(body, W, 'tgvprobe'); print('PI run', ok, '%.0f s' % dt)
ref = fits.getdata(W + 'decon_20.fits').astype(float); v = assess.valid_mask(ref)
H, Wd = ref.shape[1:]; yy, xx = np.mgrid[0:H, 0:Wd]; cx, cy = 1538, 1060; disk = np.hypot(xx - cx, yy - cy) < 120
def struct(x): L = x.mean(0); return float(np.std((ndi.gaussian_filter(L, 3) - ndi.gaussian_filter(L, 12))[disk]))
n0 = assess.hp_noise(ref.mean(0), v); s0 = struct(ref)
print('before: noise %.5f structure(3-12 px) %.5f' % (n0, s0))
for i, (s, e) in enumerate(grid):
    x = fits.getdata(W + 'tgv_%d.fits' % i).astype(float); print('  strengthL %-5s edge %-7s: noise x%.2f  structure kept %.2f  core FWHM %.2f' % (s, e, assess.hp_noise(x.mean(0), v) / n0, struct(x) / s0, assess.core_fwhm(x, v)[0]))
