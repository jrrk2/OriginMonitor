#!/usr/bin/env python3
"""Run the daemon's frame analyser (OriginHeadless frame-stats --file) over every Origin light frame of an imaging-session archive, one CSV row per frame.

    tools/archive_qc.py [--root /mnt/fits-backup/Astrophotography] [--out /mnt/fits-backup/claude/archive_qc/frames.csv]

Needs the daemon running (ORIGIN_CONTROL_PORT, default 8765); the telescope does not have to be connected. Handles Light*.fits[.fz] at the session root and in Lights/ (compressed
frames are unpacked to a temporary file with funpack). Resumable: frames already in the CSV are skipped. Session folders without light frames (calibration, Saturn videos) are ignored. --any: every FITS file in a folder is a light (the re-mosaicked TIFF snapshots).
"""
import argparse, csv, glob, json, os, subprocess, sys, tempfile, time
from astropy.io import fits

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build/headless/build/OriginHeadless')
ENV = dict(os.environ, ORIGIN_CONTROL_PORT=os.environ.get('ORIGIN_CONTROL_PORT', '8765'))
FIELDS = ['session', 'frame', 'date_obs', 'exptime', 'iso', 'ccd_temp', 'airmass', 'rot_deg', 'ra', 'dec', 'stars', 'stars_sigma10', 'star_radius_px', 'trailed', 'elongated',
          'median_adu', 'noise_adu', 'saturated_fraction', 'error']

ANY = False                                                                       # --any: every *.fits / *.fits.fz in the folder is a light (re-mosaicked TIFF snapshots)

def lights(d):
    out = []
    for pat in (('*.fits', '*.fits.fz') if ANY else ('Light*.fits', 'Light*.fits.fz', 'Lights/*.fits', 'Lights/*.fits.fz')): out += glob.glob(os.path.join(d, pat))
    return sorted(set(out))

def header(f):
    with fits.open(f) as h: return (h[1] if f.endswith('.fz') and len(h) > 1 else h[0]).header

def stats(path):
    p = subprocess.run([BIN, 'frame-stats', '--file', path], capture_output=True, text=True, env=ENV, timeout=120)
    try: return json.loads(p.stdout)
    except Exception: return {'ok': False, 'error': (p.stdout + p.stderr)[:120]}

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--root', default='/mnt/fits-backup/Astrophotography'); ap.add_argument('--out', default='/mnt/fits-backup/claude/archive_qc/frames.csv')
    ap.add_argument('--only', help='only sessions whose name contains this text'); ap.add_argument('--any', action='store_true', help='every FITS file in a folder counts as a light (tools/tiff_to_fits.py output)'); a = ap.parse_args()
    global ANY; ANY = a.any
    done = set()
    if os.path.exists(a.out):
        with open(a.out) as f: done = {(r['session'], r['frame']) for r in csv.DictReader(f)}
    new = not os.path.exists(a.out) or not done
    tmp = tempfile.mkdtemp(prefix='qc_', dir=os.path.dirname(a.out)); t0 = time.time(); n = 0
    with open(a.out, 'a', newline='') as fo:
        w = csv.DictWriter(fo, FIELDS)
        if new: w.writeheader()
        for s in sorted(os.listdir(a.root)):
            d = os.path.join(a.root, s)
            if not os.path.isdir(d) or (a.only and a.only not in s): continue
            ls = lights(d)
            if not ls: continue
            for f in ls:
                key = (s, os.path.relpath(f, d))
                if key in done: continue
                row = {'session': s, 'frame': key[1], 'error': ''}
                try:
                    h = header(f); path = f
                    if f.endswith('.fz'):
                        path = os.path.join(tmp, 'x.fits'); subprocess.run(['funpack', '-O', path, f], check=True, capture_output=True)
                    r = stats(path)
                    if f.endswith('.fz') and os.path.exists(path): os.remove(path)
                    if not r.get('ok'): row['error'] = str(r.get('error'))[:100]
                    st = r.get('streaks') or {}
                    row.update(date_obs=h.get('DATE-OBS'), exptime=h.get('EXPTIME'), iso=h.get('ISOSPEED'), ccd_temp=h.get('CCD-TEMP'), airmass=h.get('AIRMASS'), rot_deg=h.get('CROTA2'), ra=h.get('RA'), dec=h.get('DEC'),
                               stars=r.get('stars'), stars_sigma10=r.get('stars_sigma10'), star_radius_px=r.get('star_radius_px'), trailed=st.get('trailed'), elongated=st.get('elongated'),
                               median_adu=r.get('median_adu'), noise_adu=r.get('noise_adu'), saturated_fraction=r.get('saturated_fraction'))
                except Exception as e: row['error'] = repr(e)[:100]
                w.writerow(row); n += 1
                if n % 100 == 0: fo.flush(); print('%d frames, %.0f s, last %s' % (n, time.time() - t0, s), flush=True)
    print('done: %d frames in %.0f s' % (n, time.time() - t0))

if __name__ == '__main__':
    main()
