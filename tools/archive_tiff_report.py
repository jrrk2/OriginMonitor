#!/usr/bin/env python3
"""Group the re-mosaicked TIFF snapshots (tools/tiff_to_fits.py, scored by tools/archive_qc.py --any) into observing sessions by capture time, score them with the same acceptance rule as
tools/archive_report.py, and plate-solve the best frame of each session (OriginHeadless solve --file; the daemon must run) to name the target.

    tools/archive_tiff_report.py [--csv .../tiff_frames.csv] [--root /mnt/fits-backup/claude/origin_tiff_fits] [--gap 45]
"""
import argparse, csv, collections, glob, json, math, os, subprocess, tempfile
from datetime import datetime
import numpy as np

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build/headless/build/OriginHeadless')
ENV = dict(os.environ, ORIGIN_CONTROL_PORT=os.environ.get('ORIGIN_CONTROL_PORT', '8765'))
BLACK = 4985.0
OBJ = {'M31 Andromeda': (10.685, 41.27), 'M33': (23.46, 30.66), 'M42 Orion Nebula': (83.82, -5.39), 'M45 Pleiades': (56.75, 24.12), 'M1 Crab': (83.63, 22.01), 'M51 Whirlpool': (202.47, 47.2),
       'M81/M82': (148.9, 69.2), 'M101': (210.8, 54.35), 'M13': (250.42, 36.46), 'M27': (299.9, 22.72), 'NGC 7000': (314.7, 44.3), 'NGC 6992 Veil': (313.8, 31.7), 'IC 405': (79.2, 34.3),
       'NGC 2174': (92.3, 20.5), 'Double Cluster': (35.0, 57.1), 'M63 Sunflower': (198.96, 42.03), 'NGC 6946': (308.7, 60.15), 'M94': (192.7, 41.1), 'Pelican IC 5070': (312.9, 44.3),
       'M35': (92.2, 24.3), 'IC 434 Horsehead': (85.25, -2.46), 'Rosette': (97.9, 4.95), 'Soul': (43.7, 60.4), 'Heart': (38.2, 61.4), 'M8': (270.9, -24.4), 'M17': (275.2, -16.2), 'M16': (274.7, -13.8),
       'M57': (283.4, 33.03), 'M92': (259.3, 43.1), 'Perseus cluster NGC 1275': (49.95, 41.5), 'Pinwheel M33': (23.46, 30.66)}

def num(x):
    try: return float(x)
    except Exception: return float('nan')

def name(ra, dec):
    best = min(OBJ.items(), key=lambda kv: math.hypot((ra - kv[1][0]) * math.cos(math.radians(dec)), dec - kv[1][1]))
    d = math.hypot((ra - best[1][0]) * math.cos(math.radians(dec)), dec - best[1][1])
    return '%s (%.1f deg away)' % (best[0], d) if d < 5 else 'RA %.1f Dec %+.1f' % (ra, dec)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--csv', default='/mnt/fits-backup/claude/archive_qc/tiff_frames.csv'); ap.add_argument('--root', default='/mnt/fits-backup/claude/origin_tiff_fits')
    ap.add_argument('--gap', type=float, default=45); ap.add_argument('--out', default='/mnt/fits-backup/claude/archive_qc/tiff_sessions.md'); a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.csv))]
    import re
    for r in rows:
        if r['date_obs']: r['t'] = datetime.fromisoformat(r['date_obs'][:19]); continue
        m = re.search(r'(\d{8})_(\d{6})', r['frame'])                      # snapshot_YYYYMMDD_HHMMSS: local time from the file name
        r['t'] = datetime.strptime(m.group(1) + m.group(2), '%Y%m%d%H%M%S') if m else datetime(2000, 1, 1)
    rows.sort(key=lambda r: (r['session'], r['t']))
    seen = set(); uniq = []
    for r in rows:
        k = (r['t'], r['median_adu'], r['stars'])                      # the same snapshot can be in two folders
        if k in seen: continue
        seen.add(k); uniq.append(r)
    uniq.sort(key=lambda r: r['t'])
    cl = []
    for r in uniq:
        if cl and (r['t'] - cl[-1][-1]['t']).total_seconds() < a.gap * 60: cl[-1].append(r)
        else: cl.append([r])
    L = ['# Re-mosaicked TIFF snapshots grouped into sessions', '', '%d frames (%d after removing exact duplicates), %d sessions; gap rule %g min. Times are capture times as in the file (UTC for origin_temp_*, local for snapshot_*).' % (len(rows), len(uniq), len(cl), a.gap), '',
         '| start | frames | exp s | ISO | accepted | % | exposure kept s | stars | radius px | target (plate solve of the best frame) |', '|---|---|---|---|---|---|---|---|---|---|']
    tmp = tempfile.mkdtemp(prefix='tq_', dir=os.path.dirname(a.out)); out = []
    for c in cl:
        st = np.array([num(r['stars']) for r in c]); rad = np.array([num(r['star_radius_px']) for r in c]); ex = np.array([num(r['exptime']) for r in c]); sat = np.array([num(r['saturated_fraction']) for r in c])
        sky = (np.array([num(r['median_adu']) for r in c]) - BLACK) / np.maximum(ex, 1e-3); trailed = np.array([r['trailed'] == 'True' for r in c]); err = np.array([bool(r['error']) for r in c])
        p90 = np.nanpercentile(st, 90) if len(st) else 0; ref = np.nanmedian(sky)
        good = ~err & ~trailed & ~(rad > 2.6) & (st >= 25) & (st >= 0.5 * p90) & ~(sky > 1.5 * ref) & ~(sat > 0.01)
        tgt = '-'
        if good.sum() >= 5 or len(c) >= 20:
            idx = np.argsort(-(st * good))[0]; r = c[idx]; f = os.path.join(a.root, r['session'], r['frame'])
            if f.endswith('.fz') and os.path.exists(f):
                x = os.path.join(tmp, 'x.fits'); subprocess.run(['funpack', '-O', x, f], capture_output=True)
                p = subprocess.run([BIN, 'solve', '--file', x], capture_output=True, text=True, env=ENV, timeout=240)
                try: j = json.loads(p.stdout)
                except Exception: j = {}
                tgt = name(j['ra_deg'], j['dec_deg']) if j.get('solved') else 'not solved (%s)' % str(j.get('error', ''))[:40]
                os.remove(x)
        L.append('| %s | %d | %g | %s | %d | %.0f | %.0f | %.0f | %.2f | %s |' % (c[0]['t'].strftime('%Y-%m-%d %H:%M'), len(c), np.nanmedian(ex), c[0]['iso'], good.sum(), 100 * good.mean(), float((ex * good).sum()), np.nanmedian(st), np.nanmedian(rad), tgt))
        out.append((c[0]['t'], len(c), int(good.sum()), float((ex * good).sum()), tgt))
        print(L[-1], flush=True)
    open(a.out, 'w').write('\n'.join(L) + '\n')

if __name__ == '__main__':
    main()
