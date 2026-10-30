#!/usr/bin/env python3
"""Restack every target from all its good frames, across nights and stack fragments, into one final image per target and filter.

    tools/restack.py [--data /mnt/fits-backup/claude] [--out DATA/finals] [--only NAME[,NAME]] [--jobs 3] [--dry-run] [--redo]

Live stacks are fragments: a stack per name the target was given (M31 and M31_centred, NGC7000_iso200 and NGC 7000 North America Nebula), archived copies after a bad reference,
pre-filter stacks set aside, frames taken while the analyser had no target ("auto"). The raw snapshots are all kept, so this rebuilds from them:

  * frames: every row of focus_log.csv (the live analysis of each snapshot) except those rejected for a fault of the frame (no pointing, defocused, unreadable, trailed),
    matched to its snapshot_*.fits by time; cloud, haze and pointing are judged again by the stacker against the final stack's own reference
    (the file name is the local download time, the log row the UTC analysis time a moment later)
  * targets: the log's target name, grouped by pointing: names whose frames centre within 0.5 deg of each other are one target, "auto" frames join the nearest target within 0.6 deg,
    test names (Polaris_test) are dropped. The name used is the most common one with a space in it (the planner's), else the most common.
  * filter: the header's FILTER (Clear before the quad-band was fitted on 2026-10-15, Nebula after): broadband and narrowband data are stacked apart
  * stacking: tools/StackFiles --keep-reference (SkyMonitor::process, the live pipeline: verdicts, registration, ISO/exposure normalisation, outlier clip) with the target at the
    group's centre (the bulk of its frames; frames over 1 deg from it are left out). Frames that will not register to the reference are skipped; the stack is never replaced.
    The first frame is the reference: the sharpest frame (lowest HFR, 100+ stars) of the night with the most frames; the rest follow by time from it, nearest first.

Writes OUT/<name>_<filter>/: state.bin and preview.png (the stacker's), <name>_<filter>.fits (linear RGB, half resolution; tools/pi_flow/export_livestack.py),
records.jsonl (the verdict of every frame), frames.tsv (the input) and manifest.json (nights, frames offered and stacked, exposure). OUT is outside stacks/, so the planner's
coverage count does not see the finals twice.
"""
import argparse, bisect, collections, csv, glob, json, math, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
# live verdicts that say nothing against the frame itself: the stacker judges these again. Last night's live verdicts carried bugs since fixed (2026-10-28): "cloudy" from stale
# pre-filter references and the ISO fallback, "patchy" from reference stars outside a shifted field
REJECT = {'no_pointing', 'defocused', 'unreadable', 'trailed'}
DROP = {'Polaris_test', ''}
LONDON = ZoneInfo('Europe/London')


def header(path):
    from astropy.io import fits
    h = fits.getheader(path)
    return {'exp': float(h.get('EXPTIME', 0) or 0), 'iso': int(h.get('ISOSPEED', 0) or 0), 'ra': float(h.get('RA', 0) or 0), 'dec': float(h.get('DEC', 0) or 0),
            'filter': str(h.get('FILTER', 'Clear') or 'Clear'), 'w': int(h.get('NAXIS1', 0) or 0)}


def sep_deg(ra1, d1, ra2, d2):
    r = math.radians
    c = math.sin(r(d1)) * math.sin(r(d2)) + math.cos(r(d1)) * math.cos(r(d2)) * math.cos(r(ra1 - ra2))
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def centre(pts):
    """mean position of (ra, dec) degrees, through unit vectors"""
    x = y = z = 0.0
    for ra, dec in pts:
        a, d = math.radians(ra), math.radians(dec); x += math.cos(d) * math.cos(a); y += math.cos(d) * math.sin(a); z += math.sin(d)
    n = math.sqrt(x * x + y * y + z * z) or 1
    return math.degrees(math.atan2(y, x)) % 360, math.degrees(math.asin(z / n))


def robust_centre(pts):
    """centre of the bulk of the points: start from the point with the most neighbours within 0.5 deg, then the mean of those within 1 deg (a few frames taken
    elsewhere under the target's name pulled the plain mean 0.8 deg off the Double Cluster and 1.5 deg off M45)"""
    if not pts: return 0.0, 0.0
    step = max(1, len(pts) // 200)
    seed = max(pts[::step], key=lambda p: sum(1 for q in pts[::step] if sep_deg(*p, *q) < 0.5))
    near = [p for p in pts if sep_deg(*p, *seed) < 1.0]
    return centre(near or pts)


def snapshot_index(data):
    """snapshot FITS files by UTC time of their name (local time: GMT/BST)"""
    out = []
    for f in glob.glob(os.path.join(data, 'snapshot_*.fits')):
        b = os.path.basename(f)
        try: t = datetime.strptime(b[9:28], '%Y%m%d_%H%M%S_%f').replace(tzinfo=LONDON).astimezone(timezone.utc)
        except ValueError: continue
        out.append((t, f))
    out.sort()
    return out


def frames(data):
    snaps = snapshot_index(data); times = [t for t, _ in snaps]; used = set(); rows = []
    with open(os.path.join(data, 'focus_log.csv')) as fh:
        for r in csv.DictReader(fh):
            if r['verdict'] in REJECT or r['target'] in DROP: continue
            t = datetime.fromisoformat(r['time'].replace('Z', '+00:00'))
            i = bisect.bisect_left(times, t - timedelta(seconds=4)); best = None
            while i < len(snaps) and snaps[i][0] <= t + timedelta(seconds=0.5):   # downloaded (named) just before it was analysed
                if snaps[i][1] not in used and (best is None or abs((snaps[i][0] - t).total_seconds()) < abs((best[0] - t).total_seconds())): best = snaps[i]
                i += 1
            if not best: continue
            used.add(best[1])
            rows.append({'file': best[1], 'utc': t, 'target': r['target'], 'hfr': float(r['hfr_px'] or 0), 'stars': int(float(r['stars'] or 0))})
    return rows


def group(rows):
    for r in rows: r.update(header(r['file']))
    rows = [r for r in rows if r['exp'] > 0 and r['w'] == 3056 and not (r['ra'] == 0 and r['dec'] == 0)]
    by = collections.defaultdict(list)
    for r in rows: by[r['target']].append(r)
    named = {n: robust_centre([(r['ra'], r['dec']) for r in v]) for n, v in by.items() if n != 'auto'}
    # merge names whose centres are within 0.5 deg (aliases of one target), biggest first
    clusters = []
    for n in sorted(named, key=lambda n: -len(by[n])):
        for c in clusters:
            if sep_deg(*named[n], *c['pos']) < 0.5: c['names'].append(n); break
        else: clusters.append({'names': [n], 'pos': named[n]})
    for c in clusters:
        c['rows'] = [r for n in c['names'] for r in by[n]]
        spaced = [n for n in c['names'] if ' ' in n]
        c['name'] = max(spaced or c['names'], key=lambda n: len(by[n]))
    for r in by.get('auto', []):
        near = min(clusters, key=lambda c: sep_deg(r['ra'], r['dec'], *c['pos']), default=None)
        if near and sep_deg(r['ra'], r['dec'], *near['pos']) < 0.6: near['rows'].append(r); r['target'] = 'auto'
    out = []
    for c in clusters:
        for flt in sorted({r['filter'] for r in c['rows']}):
            rs = sorted((r for r in c['rows'] if r['filter'] == flt), key=lambda r: r['utc'])
            pos = robust_centre([(r['ra'], r['dec']) for r in rs]) if rs else None
            rs = [r for r in rs if sep_deg(r['ra'], r['dec'], *pos) < 1.0]           # a frame a degree from the target's centre is another pointing (slew, test), not this target
            if rs: out.append({'name': c['name'], 'aliases': c['names'], 'filter': flt, 'rows': rs, 'pos': robust_centre([(r['ra'], r['dec']) for r in rs])})
    return out


def safe(s):
    return ''.join(ch if ch.isalnum() or ch in '-_' else '_' for ch in s).strip('_')


def stack(g, out, exe, dry, redo=False):
    rs = g['rows']; nights = collections.Counter((r['utc'] - timedelta(hours=12)).date().isoformat() for r in rs)
    main = nights.most_common(1)[0][0]
    cands = [r for r in rs if (r['utc'] - timedelta(hours=12)).date().isoformat() == main and r['stars'] >= 100 and r['hfr'] > 0] or rs
    ref = min(cands, key=lambda r: r['hfr'])
    # nearest in time to the reference first: alt-az field rotation then changes a little from frame to frame and registration can follow it (fed from dusk, the frames
    # furthest from a 03:45 reference were rotated too far to register and M1 kept 37 of 336)
    order = sorted(rs, key=lambda r: abs((r['utc'] - ref['utc']).total_seconds()))
    d = os.path.join(out, safe(g['name']) + '_' + safe(g['filter']))
    info = {'name': g['name'], 'filter': g['filter'], 'aliases': g['aliases'], 'nights': dict(sorted(nights.items())), 'frames_offered': len(rs),
            'exposure_offered_s': round(sum(r['exp'] for r in rs)), 'reference': ref['file'], 'ra_deg': round(g['pos'][0], 4), 'dec_deg': round(g['pos'][1], 4), 'dir': d}
    if dry: return info
    if os.path.exists(os.path.join(d, 'manifest.json')) and not redo: return json.load(open(os.path.join(d, 'manifest.json')))
    os.makedirs(d, exist_ok=True)
    tsv = os.path.join(d, 'frames.tsv')
    with open(tsv, 'w') as f:
        for r in order: f.write('\t'.join([r['file'], str(r['exp']), str(r['iso']), r['utc'].strftime('%Y-%m-%dT%H:%M:%S'), str(r['ra']), str(r['dec'])]) + '\n')
    work = os.path.join(d, 'work'); os.makedirs(work, exist_ok=True)
    env = dict(os.environ, ORIGIN_DATA_DIR=work)
    subprocess.run([exe, tsv, '--target', g['name'], '--ra-hours', str(g['pos'][0] / 15), '--dec-deg', str(g['pos'][1]), '--lat', '52.2054', '--lon', '0.1218',
                    '--out', os.path.join(d, 'records.jsonl'), '--keep-reference'], env=env, stdout=subprocess.DEVNULL, stderr=open(os.path.join(d, 'stackfiles.log'), 'w'), check=False)
    sdir = glob.glob(os.path.join(work, 'stacks', '*'))
    stacked = exp = 0; verdicts = collections.Counter()
    for l in open(os.path.join(d, 'records.jsonl')):
        try: rec = json.loads(l)
        except ValueError: continue
        verdicts[rec.get('verdict', '?')] += 1
        if (rec.get('registration') or {}).get('stacked'): stacked += 1; exp += rec.get('exposure', 0)
    info.update(verdicts=dict(verdicts), frames_stacked_logged=stacked)
    if sdir:
        for fn in ('state.bin', 'preview.png'):
            if os.path.exists(os.path.join(sdir[0], fn)): os.replace(os.path.join(sdir[0], fn), os.path.join(d, fn))
        sys.path.insert(0, os.path.join(HERE, 'pi_flow')); from export_livestack import export
        info['stack'] = export(os.path.join(d, 'state.bin'), os.path.join(d, safe(g['name']) + '_' + safe(g['filter']) + '.fits'))
    subprocess.run(['rm', '-rf', work])
    json.dump(info, open(os.path.join(d, 'manifest.json'), 'w'), indent=1)
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', default=os.environ.get('ORIGIN_DATA_DIR', '/mnt/fits-backup/claude')); ap.add_argument('--out')
    ap.add_argument('--only', default=''); ap.add_argument('--jobs', type=int, default=3); ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--redo', action='store_true', help='rebuild groups that already have a manifest.json (default: skip them)')
    ap.add_argument('--stackfiles', default=os.path.join(ROOT, 'build', 'stackfiles', 'StackFiles'))
    a = ap.parse_args(); out = a.out or os.path.join(a.data, 'finals')
    groups = group(frames(a.data))
    if a.only: groups = [g for g in groups if any(o.lower() in (g['name'] + ' ' + ' '.join(g['aliases'])).lower() for o in a.only.split(','))]
    groups.sort(key=lambda g: -len(g['rows']))
    with ThreadPoolExecutor(max(1, a.jobs)) as ex:
        res = list(ex.map(lambda g: stack(g, out, a.stackfiles, a.dry_run, a.redo), groups))
    for r in res:
        st = r.get('stack', {})
        print(f"{r['name']:32s} {r['filter']:6s} nights {len(r['nights'])}  offered {r['frames_offered']:4d} ({r['exposure_offered_s'] / 60:5.0f} min)" +
              (f"  stacked {st.get('frames', 0):4d} ({st.get('total_exposure_s', 0) / 60:5.0f} min)" if st else '') + f"  aliases {','.join(r['aliases'])}")
    if not a.dry_run: json.dump(res, open(os.path.join(out, 'summary.json'), 'w'), indent=1, default=str)


if __name__ == '__main__':
    main()
