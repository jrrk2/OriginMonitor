#!/usr/bin/env python3
"""Data for the planner page of OriginWatch: for the night in progress, every candidate target's visibility through the dark hours.

    tools/planner_data.py [--out DIR] [--date YYYY-MM-DD] [--top 60]

Writes DIR/planner.json (default: the data dir, /mnt/fits-backup/claude) and copies tools/planner.html next to it as DIR/planner.html; OriginWatch serves both at /planner and /planner.json
(read-only). Each planned block carries a thumbnail of our best stack of that target so far (the restacked finals, else a live stack; matched by position), inline as a
data URI so the page needs no image route. The planner's own numbers come from `OriginHeadless schedule` (windows after the horizon file, learned glare bins, Moon and priority) and the geometry (altitude, azimuth, the
field-rotation limit on a sub) is computed here for every 15 minutes of the night. Run it every few minutes (cron) or by hand; it never talks to the telescope except to read the
current target, stack and capture status from the daemon.
"""
import argparse, base64, datetime as dt, glob, io, json, math, os, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE); sys.path.insert(0, ROOT)
LAT, LON = 52.2, 0.0                         # site (the schedule reports its own: used when available)
CORNER_PX = 0.5 * math.hypot(3056, 2048)     # field-rotation limit: 1 px of smear at the frame corner


def jd(t): return t.replace(tzinfo=dt.timezone.utc).timestamp() / 86400.0 + 2440587.5


def lst_deg(t, lon):
    d = jd(t) - 2451545.0
    return (280.46061837 + 360.98564736629 * d + lon) % 360


def altaz(ra_h, dec, t, lat, lon):
    ha = math.radians(lst_deg(t, lon) - ra_h * 15); d = math.radians(dec); la = math.radians(lat)
    sa = math.sin(d) * math.sin(la) + math.cos(d) * math.cos(la) * math.cos(ha); alt = math.asin(max(-1, min(1, sa)))
    az = math.atan2(-math.sin(ha) * math.cos(d), math.sin(d) * math.cos(la) - math.cos(d) * math.sin(la) * math.cos(ha))
    return math.degrees(alt), math.degrees(az) % 360


def tmax(alt, az, lat):
    """longest sub (s) before a corner star smears by 1 px: rotation rate of an alt-az mount is 15 deg/h cos(lat) cos(az)/cos(alt); None = unlimited"""
    r = 15.0 * math.cos(math.radians(lat)) * math.cos(math.radians(az)) / max(math.cos(math.radians(alt)), 1e-3)
    if abs(r) < 1e-3: return None
    return min(2 * (math.degrees(1 / CORNER_PX) / abs(r)) * 3600, 3600.0)


def read_filter():
    """the hand-fitted filter as the planner sees it (~/.config/origin/filter: clear | nebula; no file = clear)"""
    try: return open(os.path.expanduser('~/.config/origin/filter')).read().strip().lower() or 'clear'
    except OSError: return 'clear'


def sh(cmd, timeout=60):
    env = dict(os.environ, ORIGIN_CONTROL_PORT=os.environ.get('ORIGIN_CONTROL_PORT', '8765'))
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env, cwd=ROOT)
    return json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {}


def sep_deg(ra1_h, dec1, ra2_h, dec2):
    a1, d1, a2, d2 = math.radians(ra1_h * 15), math.radians(dec1), math.radians(ra2_h * 15), math.radians(dec2)
    return math.degrees(math.acos(max(-1.0, min(1.0, math.sin(d1) * math.sin(d2) + math.cos(d1) * math.cos(d2) * math.cos(a1 - a2)))))


def our_picture(data, ra_h, dec, filt='clear', width=480):
    """our best stack of the target at (ra_h, dec): the restacked final (DATA/finals/*/manifest.json) within 0.5 deg with the most exposure, a stack
    taken through the filter now fitted counting 2.5 times; returns {thumb: data URI, note} or {}"""
    best = None
    for m in glob.glob(os.path.join(data, 'finals', '*', 'manifest.json')):
        try:
            j = json.load(open(m)); s = j.get('stack') or {}
            if j.get('ra_deg') is None or not s.get('frames'): continue
            if sep_deg(j['ra_deg'] / 15, j['dec_deg'], ra_h, dec) > 0.5: continue
            sec = s.get('total_exposure_s', 0); rank = sec * (1.0 if (s.get('ref_filter') == 'Nebula') == (filt == 'nebula') else 0.4)   # the fitted filter's stack unless the other has far more
            if best is None or rank > best[0]:
                best = (rank, os.path.join(os.path.dirname(m), 'preview.png'), '%d frames, %d min, %s, nights %s' % (s['frames'], round(sec / 60), 'quad-band filter' if s.get('ref_filter') == 'Nebula' else 'no filter', ', '.join(sorted(j.get('nights') or {}))))
        except (OSError, ValueError, KeyError): continue
    if best is None or not os.path.exists(best[1]): return {}
    try:
        from PIL import Image
        im = Image.open(best[1]).convert('RGB'); im.thumbnail((width, width))
        b = io.BytesIO(); im.save(b, 'JPEG', quality=80)
        return {'thumb': 'data:image/jpeg;base64,' + base64.b64encode(b.getvalue()).decode(), 'thumb_note': 'our stack: ' + best[2]}
    except Exception: return {}


def tonights_run(data, night):
    """the plan the unattended run is following (run_state.json, kept up to date by the run, replans included) while that run is alive, and the blocks it has started,
    from its log after the last run_start: {key: start line}. (None, {}) when no run of this night is running: the page then shows the schedule's plan."""
    try:
        st = json.load(open(os.path.join(data, 'run_state.json')))
        if st.get('night') != night or st.get('phase') in ('finished', 'parked'): return None, {}
        os.kill(int(st['pid']), 0)
    except (OSError, ValueError, KeyError, TypeError): return None, {}
    started = {}
    try:
        for l in open(st.get('log') or os.path.join(data, 'run_%s.jsonl' % night.replace('-', ''))):
            try: j = json.loads(l)
            except ValueError: continue
            if j.get('decision') == 'run_start': started = {}
            elif j.get('decision') == 'start' and j.get('target'): started.setdefault(j['target'], j)
    except OSError: pass
    return st.get('plan') or [], started


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default='/mnt/fits-backup/claude'); ap.add_argument('--date'); ap.add_argument('--top', type=int, default=60); ap.add_argument('--step', type=int, default=15); a = ap.parse_args()
    exe = os.path.join(ROOT, 'build/headless/build/OriginHeadless')
    cmd = [exe, 'schedule', '--top', str(a.top), '--sky-map', '--novelty', 'on', '--weather', 'on', '--min-quality', '0.2'] + (['--date', a.date] if a.date else [])
    s = sh(cmd, 120)
    if not s.get('ok'): sys.exit('schedule failed: %s' % s)
    site = s.get('site') or {}; lat = float(site.get('lat', LAT)); lon = float(site.get('lon', LON))
    fmt = '%Y-%m-%dT%H:%M:%SZ'; t0 = dt.datetime.strptime(s['dark_start'][:19] + 'Z', fmt) - dt.timedelta(minutes=60); t1 = dt.datetime.strptime(s['dark_end'][:19] + 'Z', fmt) + dt.timedelta(minutes=30)
    times = []; t = t0
    while t <= t1: times.append(t); t += dt.timedelta(minutes=a.step)
    stacks = {}
    sd = os.path.join(a.out, 'stacks')
    if os.path.isdir(sd):
        for n in sorted(os.listdir(sd)):
            p = os.path.join(sd, n)
            if os.path.isdir(p) and os.path.exists(os.path.join(p, 'state.bin')): stacks[n] = {'mtime': dt.datetime.fromtimestamp(os.path.getmtime(os.path.join(p, 'state.bin')), dt.timezone.utc).strftime(fmt)}
    targets = []
    for tg in s['targets']:
        rows = []
        for t in times:
            alt, az = altaz(tg['ra_hours'], tg['dec_deg'], t, lat, lon); tm = tmax(alt, az, lat)
            rows.append([round(alt, 1), round(az), None if tm is None else round(tm, 1)])
        targets.append({k: tg.get(k) for k in ('key', 'name', 'ra_hours', 'dec_deg', 'priority', 'novelty', 'size_arcmin', 'window_start', 'window_end', 'minutes', 'peak', 'peak_alt', 'moon_fit', 'faintness')} | {'rows': rows})
    try:
        live = {'target': sh([exe, 'target'])}
        seq = sh([exe, 'sequence']); live['sequence'] = {k: (seq.get('status') or {}).get(k) for k in ('active', 'frames', 'good', 'bad', 'exposure', 'iso')}
        live['mount'] = {k: v for k, v in (sh([exe, 'mount']).get('mount') or {}).items() if k in ('IsAligned', 'NumAlignRefs', 'IsTracking', 'Ra', 'Dec')}
        live['mount']['IsReady'] = (((sh([exe, 'status']).get('last') or {}).get('TaskController/GetStatus') or {}).get('IsReady'))   # the firmware's "initialised" flag
    except Exception as e:
        live = {'error': str(e)}
    by_key = {t['key']: t for t in targets}
    run_plan, started = tonights_run(a.out, s.get('night'))
    plan = []
    for b in run_plan if run_plan is not None else s.get('plan', []):
        blk = {k: b.get(k) for k in ('key', 'name', 'start', 'end', 'priority', 'minutes')}
        if b.get('key') in started: blk.update(started=started[b['key']]['time'], until=started[b['key']].get('until'), backup=started[b['key']].get('backup'))
        t = by_key.get(b.get('key')) or b
        if t.get('ra_hours') is not None:
            blk.update(ra_hours=t['ra_hours'], dec_deg=t['dec_deg'], size_arcmin=t.get('size_arcmin'), novelty=t.get('novelty'))
            blk.update(our_picture(a.out, t['ra_hours'], t['dec_deg'], read_filter()))
        plan.append(blk)
    wx = [{'utc': h['utc'], 'quality': h.get('quality'), 'cloud': h.get('cloud'), 'precip_mm': h.get('precip_mm')} for h in (s.get('weather') or {}).get('hours', [])]
    out = {'generated': dt.datetime.now(dt.timezone.utc).strftime(fmt), 'night': s.get('night'), 'site': {'lat': lat, 'lon': lon}, 'dark_start': s['dark_start'], 'dark_end': s['dark_end'],
           'moon_illumination': s.get('moon_illumination'), 'step_min': a.step, 'times': [t.strftime(fmt) for t in times], 'targets': targets,
           'plan': plan, 'plan_source': 'run' if run_plan is not None else 'schedule', 'weather': wx, 'stacks': stacks, 'live': live,
           'horizon': s.get('horizon'), 'filter': read_filter()}
    os.makedirs(a.out, exist_ok=True); tmp = os.path.join(a.out, 'planner.json.tmp'); json.dump(out, open(tmp, 'w')); os.replace(tmp, os.path.join(a.out, 'planner.json'))
    shutil.copyfile(os.path.join(HERE, 'planner.html'), os.path.join(a.out, 'planner.html'))
    print('planner.json: %d targets, %d time steps, %d plan blocks' % (len(targets), len(times), len(out['plan'])))


if __name__ == '__main__': main()
