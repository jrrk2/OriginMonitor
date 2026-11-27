#!/usr/bin/env python3
"""Move CCDciel captures (Bayer mosaics from tools/rgb_to_bayer.py) into OriginMonitor's capture area and restack them with the live pipeline.

    tools/import_ccdciel.py SRC_DIR TARGET [--data /mnt/fits-backup/claude] [--no-move] [--stack] [--final-name NAME] [--redo]

SRC_DIR is searched recursively for .fits/.fit and fpacked .fits.fz/.fit.fz; frames already imported (same capture second) are left where they are.

CCDciel/INDI frames differ from the Origin's own snapshots, so each is rewritten on the way in:
  * rows: CCDciel writes ROWORDER=TOP-DOWN, the Origin bottom-up; flipped up-down. Checked on M101 (2026-11-24): the factory hot-pixel map lands on these
    frames only after the flip (and there 83 ADU *below* the neighbours: the camera had already taken hot pixels out, so no dark calibration for these)
  * black level already off (sky ~1000-1800 ADU, none near the Origin's 4985): stacked with StackFiles --black 0
  * header: ISOSPEED from GAIN, BAYERPAT RGGB as the Origin writes it, DATE-OBS kept (UTC), FILTER and FILTDTCT Clear: every CCDciel capture predates the
    tri-band filter (bought 2026-10-14). The stars' colour would say otherwise (sky B/G 1.1, R/G 0.63 against about 0.6 for the Origin's Clear frames: the
    older firmware's colour balance), so FILTDTCT, which SkyMonitor takes before its own detection, fixes the stack's filter and with it the Gaia colour model
  * name: DATA/frames/TARGET/snapshot_<local time>_<ms>.fits as the daemon names them; the source file is removed once the copy reads back identical

Origin app saves (CREATOR 'Origin x.y', one plane) are imported too: mirrored left-right, DATE-OBS from local time to UTC, BLACKLVL 4985 (black still in);
INDI frames get BLACKLVL 0. StackFiles takes each frame's BLACKLVL, so the two can share a stack.

--stack then restacks every imported frame of TARGET in DATA/frames/TARGET (header CCDCIEL or ORIGAPP) through tools/restack.py's stack():
reference the sharpest of the starriest (SkyFrame's view of the quick star count's top 12), into DATA/finals/<NAME>_<filter seen in the reference's stars>, NAME the
--final-name (default TARGET; "<TARGET> CCDciel" keeps them apart from the Origin's own finals of the same target, which tools/restack.py rebuilds from focus_log).
"""
import argparse, glob, os, sys
from datetime import datetime, timezone
from zoneinfo import ZoneInfo
import numpy as np
from astropy.io import fits

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import restack

LONDON = ZoneInfo('Europe/London')


def import_one(src, dst_dir, move):
    with fits.open(src) as hl:
        hdu = hl[1] if len(hl) > 1 and hl[0].data is None else hl[0]          # fpacked: the image is the compressed extension
        h = hdu.header.copy(); d = hdu.data
        app = str(h.get('CREATOR', '')).startswith('Origin ') and h.get('NAXIS') == 2
        if app:
            # the Origin app's own saves (Light0001.fits, firmware 1.2-1.5, 2025-10 to 2026-09): one Bayer plane like a snapshot, black and hot pixels
            # still in, but mirrored left-right (the factory hot-pixel map lands only after that flip: +135 ADU on M33, +78 on M1) and DATE-OBS local time
            if str(h.get('IMAGETYP', '')).strip().upper() != 'LIGHT': return None, 'skip (not a light frame)'
            d = np.fliplr(d).astype(np.uint16)
        elif h.get('NAXIS') != 2 or str(h.get('BAYERPAT', '')).strip() != 'BGGR': return None, 'skip (not a converted BGGR mosaic)'
        else: d = np.flipud(d).astype(np.uint16)
    utc = datetime.fromisoformat(h['DATE-OBS'])
    if app:
        tz = str(h.get('TIMEZONE', '+0000')).strip(); sgn = -1 if tz.startswith('-') else 1
        utc = utc - sgn * restack.timedelta(hours=int(tz[1:3]), minutes=int(tz[3:5]))
    utc = utc.replace(tzinfo=timezone.utc)
    loc = utc.astimezone(LONDON)
    name = f"snapshot_{loc:%Y%m%d_%H%M%S}_{loc.microsecond // 1000:03d}.fits"
    dst = os.path.join(dst_dir, name)
    if os.path.exists(dst): return dst, 'exists'
    for k in ('ROWORDER', 'XBAYROFF', 'YBAYROFF', 'XTENSION', 'PCOUNT', 'GCOUNT', 'EXTNAME'): h.remove(k, ignore_missing=True)
    h['BAYERPAT'] = ('RGGB', 'as the Origin writes it (greens on the main diagonal as read)')
    if not app: h['ISOSPEED'] = (int(h.get('GAIN', 0) or 0), 'from CCDciel GAIN')
    if utc < datetime(2026, 10, 14, tzinfo=timezone.utc):
        h['FILTER'] = ('Clear', 'no filter: the tri-band was bought 2026-10-14')
        h['FILTDTCT'] = ('Clear', 'as FILTER (the old firmware\'s colour misleads the star test)')
    if app:
        h['DATE-OBS'] = (utc.strftime('%Y-%m-%dT%H:%M:%S'), 'UTC start of exposure (the app wrote local time)'); h['TIMEZONE'] = '+0000'
        h['BLACKLVL'] = (4985, 'black level, ADU (StackFiles reads it per frame)')
        h['ORIGAPP'] = (os.path.relpath(src, os.path.dirname(os.path.dirname(src)))[:60], 'imported Origin app save, mirrored left-right')
        h['HISTORY'] = 'Imported from an Origin app save by tools/import_ccdciel.py: columns mirrored to the snapshots\' layout'
    else:
        h['BLACKLVL'] = (0, 'black level, ADU: already off in INDI captures')
        h['CCDCIEL'] = (os.path.basename(src), 'imported CCDciel/INDI capture, flipped up-down, black already off')
        h['HISTORY'] = 'Imported from CCDciel by tools/import_ccdciel.py: rows flipped to bottom-up'
    tmp = dst + '.tmp'
    fits.PrimaryHDU(d, header=h).writeto(tmp, output_verify='silentfix')
    if not np.array_equal(fits.getdata(tmp), d): os.remove(tmp); return None, 'skip (write check failed)'
    os.rename(tmp, dst)
    if move: os.remove(src)
    return dst, 'ok'


def quick_stars(path):
    """stars over 10 sigma and a rough HFR of the brightest 50 unsaturated, on the 2x2-binned frame"""
    from scipy.ndimage import maximum_filter, median_filter
    a = fits.getdata(path).astype(np.float32); a = a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]
    a = a - median_filter(a[::8, ::8], 9).repeat(8, 0).repeat(8, 1)[:a.shape[0], :a.shape[1]]
    s = 1.4826 * np.median(np.abs(a[::4, ::4] - np.median(a[::4, ::4])))
    pk = (a == maximum_filter(a, 7)) & (a > 10 * s) & (a < 0.8 * a.max())
    pk[:8] = pk[-8:] = False; pk[:, :8] = pk[:, -8:] = False
    ys, xs = np.nonzero(pk); o = np.argsort(-a[ys, xs])[:50]; hf = []
    br = np.argsort(-a[ys, xs])[:300]; peaks = np.stack([ys[br], xs[br]], 1).astype(np.float32)
    yy, xx = np.mgrid[-6:7, -6:7]; r = np.hypot(yy, xx).ravel(); ordr = np.argsort(r)
    for y, x in zip(ys[o], xs[o]):
        c = np.clip(a[y - 6:y + 7, x - 6:x + 7].ravel(), 0, None)[ordr]; cs = np.cumsum(c)
        if cs[-1] > 0: hf.append(r[ordr][np.searchsorted(cs, cs[-1] / 2)])
    return len(ys), float(np.median(hf)) * 2 if hf else 99.0, peaks


def confirmed(peaks, others, tol=4.0):
    """fraction of a frame's peaks found again (within tol binned px, after the median offset) in frames taken minutes either side: a satellite or plane trail,
    a cosmic ray or a hot cluster is in one frame only. A reference with a trail made 1253 of 2623 M51 frames read patchy (2026-11-24): its trail's peaks
    counted as reference stars the other frames lacked."""
    from scipy.spatial import cKDTree
    fr = []
    for o in others:
        if len(o) < 10 or len(peaks) < 10: continue
        t = cKDTree(o); d, i = t.query(peaks, distance_upper_bound=40)
        ok = np.isfinite(d)
        if ok.sum() < 10: continue
        off = np.median(o[i[ok]] - peaks[ok], axis=0)
        d2, _ = t.query(peaks + off, distance_upper_bound=tol)
        fr.append(np.mean(np.isfinite(d2)))
    return float(np.median(fr)) if fr else 0.0


def probe(path, exe):
    """SkyFrame's own view of one frame (one StackFiles pass): stars, HFR, the filter seen in the stars' colour"""
    import json, subprocess, tempfile
    with tempfile.TemporaryDirectory() as t:
        h = restack.header(path); tsv = os.path.join(t, 'l.tsv')
        open(tsv, 'w').write('\t'.join([path, str(h['exp']), str(h['iso']), '2026-01-01T00:00:00', str(h['ra']), str(h['dec'])]) + '\n')
        subprocess.run([exe, tsv, '--out', os.path.join(t, 'r.jsonl'), '--black', '0', '--ra-hours', str(h['ra'] / 15), '--dec-deg', str(h['dec'])],
                       env=dict(os.environ, ORIGIN_DATA_DIR=t), capture_output=True)
        try: r = json.loads(open(os.path.join(t, 'r.jsonl')).readline())
        except (OSError, ValueError): return 0, 99.0, 'Unknown'
        return int(r.get('stars') or 0), float(r.get('hfr_px') or 99), r.get('filter_detected') or 'Unknown'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src', nargs='?'); ap.add_argument('target')
    ap.add_argument('--data', default=os.environ.get('ORIGIN_DATA_DIR', '/mnt/fits-backup/claude'))
    ap.add_argument('--final-name', help='name of the restack (DATA/finals/<NAME>_<filter>; default TARGET)')
    ap.add_argument('--no-move', action='store_true'); ap.add_argument('--stack', action='store_true'); ap.add_argument('--redo', action='store_true')
    ap.add_argument('--stackfiles', default=os.path.join(restack.ROOT, 'build', 'stackfiles', 'StackFiles'))
    a = ap.parse_args()
    dst_dir = os.path.join(a.data, 'frames', restack.safe(a.target)); os.makedirs(dst_dir, exist_ok=True)
    if a.src:
        n = {}
        for f in sorted(f for e in ('*.fits', '*.fit', '*.fits.fz', '*.fit.fz') for f in glob.glob(os.path.join(a.src, '**', e), recursive=True)):
            if '_rgb_originals' in f: continue
            _, r = import_one(f, dst_dir, not a.no_move); n[r.split(' ')[0]] = n.get(r.split(' ')[0], 0) + 1
            if r != 'ok': print(f, r)
        print(a.target, n, '->', dst_dir)
    if not a.stack: return
    rows = []
    for f in sorted(glob.glob(os.path.join(dst_dir, 'snapshot_*.fits'))):
        h = fits.getheader(f)
        if 'CCDCIEL' not in h and 'ORIGAPP' not in h: continue
        r = restack.header(f); r.update(file=f, utc=datetime.fromisoformat(h['DATE-OBS']).replace(tzinfo=timezone.utc), target=a.target, verdict='')
        rows.append(r)
    from concurrent.futures import ProcessPoolExecutor
    with ProcessPoolExecutor(8) as ex:
        for r, (ns, hfr, pk) in zip(rows, ex.map(quick_stars, [r['file'] for r in rows])): r['stars'], r['hfr'], r['peaks'] = ns, hfr, pk
    rows = [r for r in rows if r['exp'] >= 1]          # planetary bursts are not for this stacker
    if not rows: print('nothing to stack'); return
    pos = restack.robust_centre([(r['ra'], r['dec']) for r in rows])
    rows = [r for r in rows if restack.sep_deg(r['ra'], r['dec'], *pos) < 1.0]
    # reference: the quick count's 12 starriest frames of the biggest night, judged by SkyFrame itself (a trail or a bright edge can fool the quick count: a frame it
    # ranked first had 45 stars to SkyFrame and refused the other 39 as patchy); the sharpest with the most stars. stack() takes the lowest 'hfr' with 100+ stars.
    night = lambda r: (r['utc'] - restack.timedelta(hours=12)).date()
    import collections
    main_night = collections.Counter(night(r) for r in rows).most_common(1)[0][0]
    mn = sorted((r for r in rows if night(r) == main_night), key=lambda r: r['utc'])
    # only frames whose stars are confirmed by their neighbours in time (no trail), the 12 starriest of those
    for i, r in enumerate(mn): r['confirmed'] = confirmed(r['peaks'], [o['peaks'] for o in mn[max(0, i - 3):i] + mn[i + 1:i + 4]])
    cands = sorted((r for r in mn if r['confirmed'] >= 0.85), key=lambda r: -r['stars'])[:12] or sorted(mn, key=lambda r: -r['confirmed'])[:12]
    print(f"reference candidates: {sum(r['confirmed'] >= 0.85 for r in mn)} of {len(mn)} frames of {main_night} have 85%+ of their stars in the frames either side", flush=True)
    for r in rows: r['hfr'] = 0; r['stars'] = 0; r.pop('peaks', None)
    with ProcessPoolExecutor(6) as ex:
        for r, (ns, hfr, flt) in zip(cands, ex.map(probe, [r['file'] for r in cands], [a.stackfiles] * len(cands))): r['stars'], r['hfr'], r['filter'] = ns, hfr, flt
    top = max(r['stars'] for r in cands)
    best = min((r for r in cands if r['stars'] >= 0.9 * top), key=lambda r: r['hfr'])
    for r in cands:
        if r is not best: r['stars'] = 0
    flt = best['filter']
    print(f"reference {os.path.basename(best['file'])}: {best['stars']} stars, HFR {best['hfr']:.2f} px, filter {flt}")
    g = {'name': a.final_name or a.target, 'aliases': [a.target], 'filter': flt, 'rows': rows, 'pos': pos}
    info = restack.stack(g, os.path.join(a.data, 'finals'), a.stackfiles, False, a.redo, extra=('--black', '0'))
    print({k: info.get(k) for k in ('name', 'nights', 'frames_offered', 'frames_stacked_logged', 'verdicts', 'reference', 'dir')})


if __name__ == '__main__':
    main()
