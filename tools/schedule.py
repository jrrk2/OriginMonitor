#!/usr/bin/env python3
"""Observing schedule for one night from Cambridge UK: popular targets, a local horizon (buildings, trees), Sun and Moon.

    tools/schedule.py                       # tonight
    tools/schedule.py --date 2026-10-10 --horizon tools/horizon.json --min-alt 25

A target is observable while it is above the local horizon at its azimuth (not just above 0 deg), the Sun is below --sun-alt
(default -18, astronomical dark) and it is farther than --moon-sep from the Moon. Targets are listed with their windows, then
a greedy one-target-at-a-time plan is built: at each step, take the highest-priority target that can be imaged for at least
--min-block minutes, keep it until it sets behind the horizon or its altitude is beaten by a better one, and so on.

Horizon: JSON {"points": [[az_deg, min_alt_deg], ...]} interpolated circularly in azimuth (north = 0, east = 90). Without a file the
horizon is flat at --min-alt (a placeholder: measure your real one!). --sky-map additionally raises the horizon in every 10 deg x 5 deg bin
that the app's SkyMonitor has learned is obstructed (/mnt/fits-backup/claude/sky_map.json, "likely_obstructed" rule of obstructionMap()).
Messier RA/Dec come from MessierCatalog.hpp; the curated non-Messier extras are below.
"""
import argparse, json, os, re, sys, warnings
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
import numpy as np
warnings.filterwarnings('ignore')
import astropy.units as u
from astropy.utils import iers
iers.conf.auto_download = False; iers.conf.auto_max_age = None
from astropy.coordinates import AltAz, EarthLocation, SkyCoord, get_body, get_sun
from astropy.time import Time

SITE = EarthLocation(lat=52.2054 * u.deg, lon=0.1218 * u.deg, height=20 * u.m)
HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL = ZoneInfo('Europe/London')

# popularity weight 1-10: how rewarding the object is for a 152 mm f/2.2 wide-field imager. Messier objects not listed get 3.
PRIORITY = {'M31': 10, 'M42': 10, 'M45': 9, 'M33': 8, 'M27': 8, 'M57': 7, 'M13': 8, 'M51': 8, 'M81': 7, 'M82': 7, 'M101': 6, 'M8': 8, 'M20': 7,
            'M17': 8, 'M16': 7, 'M1': 6, 'M44': 6, 'M3': 6, 'M92': 5, 'M15': 6, 'M2': 5, 'M52': 4, 'M103': 4, 'M35': 5, 'M36': 4, 'M37': 4, 'M38': 4,
            'M39': 4, 'M34': 4, 'M76': 4, 'M97': 4, 'M65': 4, 'M66': 4, 'M64': 4, 'M63': 4, 'M104': 5, 'M87': 4, 'M106': 5, 'M5': 5, 'M11': 5,
            'M110': 4, 'M32': 4, 'M41': 5, 'M46': 4, 'M47': 4, 'M67': 4, 'M93': 4, 'M50': 3}
# RA hours, Dec degrees, size arcmin
EXTRAS = {
    'NGC 7000 North America Nebula': (20.9783, 44.33, 120, 9), 'IC 5070 Pelican Nebula': (20.8, 44.35, 60, 7),
    'NGC 6992 Eastern Veil': (20.9367, 31.72, 60, 9), 'NGC 6960 Western Veil': (20.76, 30.72, 70, 8),
    'NGC 869/884 Double Cluster': (2.35, 57.13, 60, 8), 'IC 1805 Heart Nebula': (2.5333, 61.45, 60, 7), 'IC 1848 Soul Nebula': (2.8833, 60.43, 60, 6),
    'NGC 281 Pacman Nebula': (0.8767, 56.62, 35, 6), 'IC 1396 Elephant Trunk': (21.65, 57.5, 170, 7), 'NGC 7380 Wizard Nebula': (22.78, 58.1, 25, 5),
    'NGC 7293 Helix Nebula': (22.4947, -20.84, 25, 6), 'NGC 891': (2.37, 42.35, 13, 5), 'NGC 253 Sculptor Galaxy': (0.7925, -25.29, 27, 6),
    'NGC 1499 California Nebula': (4.0, 36.42, 145, 7), 'NGC 2244 Rosette Nebula': (6.5333, 4.95, 80, 8), 'NGC 7662 Blue Snowball': (23.43, 42.54, 0.5, 3),
    'NGC 457 Owl Cluster': (1.32, 58.3, 13, 4), 'NGC 7822 Cepheus Nebula': (0.0, 68.0, 60, 4), 'NGC 6888 Crescent Nebula': (20.2, 38.35, 20, 6),
    'NGC 7331 + Stephan Quintet': (22.62, 34.4, 10, 5), 'NGC 6946 Fireworks Galaxy': (20.58, 60.15, 11, 5), 'NGC 4565 Needle Galaxy': (12.6, 25.99, 16, 4),
    'NGC 2264 Cone/Christmas Tree': (6.68, 9.9, 60, 6), 'Mel 20 Alpha Persei Cluster': (3.4, 49.0, 300, 4),
    'IC 434 Horsehead / NGC 2024 Flame': (5.6833, -2.4, 90, 8), 'NGC 1333': (3.4, 31.4, 10, 4), 'NGC 7789 Caroline\'s Rose': (23.95, 56.7, 15, 4),
}

def load_messier():
    t = open(os.path.join(HERE, '..', 'MessierCatalog.hpp'), encoding='utf-8', errors='replace').read()
    out = []
    for m in re.finditer(r'\{\s*(\d+),\s*"(M\d+)",\s*"([^"]*)",\s*MessierObjectType::(\w+),[^\n]*\n\s*createSkyPosition\(([-\d.]+),\s*([-\d.]+),[^\n]*\n\s*[^,]*,[^,]*,\s*QSizeF\(([\d.]+),', t):
        _, n, common, typ, ra, dec, size = m.groups()
        out.append(dict(name=n + (' ' + common if common else ''), key=n, ra=float(ra), dec=float(dec), size=float(size), prio=PRIORITY.get(n, 3), type=typ))
    return out

def load_targets():
    ts = load_messier()
    if len(ts) < 100: sys.exit(f'only parsed {len(ts)} Messier objects from MessierCatalog.hpp; parser needs updating')
    for k, (ra, dec, size, p) in EXTRAS.items(): ts.append(dict(name=k, key=k, ra=ra, dec=dec, size=size, prio=p, type='EXTRA'))
    return ts

class Horizon:
    def __init__(self, path, flat, sky_map):
        self.flat = flat; self.pts = None; self.bins = {}
        if path:
            self.pts = sorted(json.load(open(path))['points']); self.az = np.array([p[0] for p in self.pts], float); self.alt = np.array([p[1] for p in self.pts], float)
        if sky_map and os.path.exists(sky_map):
            for k, b in json.load(open(sky_map)).get('bins', {}).items():
                good, bad, nights = b.get('good', 0), b.get('bad', 0), len(b.get('bad_dates', []))
                if good == 0 and bad >= 3 and nights >= 2:
                    a, h = (int(x) for x in k.split('_')); self.bins[(a, h)] = True
    def min_alt(self, az):
        az = np.asarray(az, float) % 360
        if self.pts is None: h = np.full_like(az, self.flat)
        else: h = np.interp(az, np.r_[self.az - 360, self.az, self.az + 360], np.r_[self.alt, self.alt, self.alt])
        for (a, hb) in self.bins:   # an obstructed bin blocks everything up to its top edge
            h = np.where((az >= a) & (az < a + 10), np.maximum(h, hb + 5), h)
        return h

def hhmm(t): return t.astimezone(LOCAL).strftime('%H:%M')

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--date', help='local evening date YYYY-MM-DD (default today)'); ap.add_argument('--horizon', default=os.path.join(HERE, 'horizon.json'))
    ap.add_argument('--min-alt', type=float, default=20, help='flat horizon / floor when no horizon file (deg)')
    ap.add_argument('--sun-alt', type=float, default=-18); ap.add_argument('--moon-sep', type=float, default=30)
    ap.add_argument('--min-block', type=float, default=45, help='shortest useful block, minutes'); ap.add_argument('--step', type=float, default=5, help='minutes')
    ap.add_argument('--sky-map', nargs='?', const=os.path.join(os.environ.get('ORIGIN_DATA_DIR', '/mnt/fits-backup/claude'), 'sky_map.json'), help='also use learned obstruction bins')
    ap.add_argument('--max-block', type=float, default=150, help='longest time on one target, minutes'); ap.add_argument('--json', action='store_true', help='machine-readable output for tools/nightrun.py'); ap.add_argument('--top', type=int, default=25); a = ap.parse_args()

    real = sys.stdout
    if a.json: sys.stdout = open(os.devnull, 'w')
    day = datetime.strptime(a.date, '%Y-%m-%d').date() if a.date else datetime.now(LOCAL).date()
    t0 = datetime(day.year, day.month, day.day, 17, 0, tzinfo=LOCAL)
    times = [t0 + timedelta(minutes=a.step * i) for i in range(int(14 * 60 / a.step))]     # 17:00 to 07:00
    T = Time([t.astimezone(timezone.utc).replace(tzinfo=None) for t in times], scale='utc'); frame = AltAz(obstime=T, location=SITE)
    sun = get_sun(T).transform_to(frame).alt.deg; moon_c = get_body('moon', T, SITE); moon = moon_c.transform_to(frame).alt.deg
    elong = get_sun(T).separation(moon_c).deg; illum = (1 - np.cos(np.radians(elong))) / 2      # approximate illuminated fraction
    dark = sun < a.sun_alt
    hz = Horizon(a.horizon if os.path.exists(a.horizon) else None, a.min_alt, a.sky_map)
    src = f'horizon file {a.horizon}' if hz.pts is not None else f'flat {a.min_alt:g} deg PLACEHOLDER (no {a.horizon})'
    print(f'Night of {day} from Cambridge ({SITE.lat.deg:.3f}N, {SITE.lon.deg:.3f}E); horizon: {src}' + (f' + {len(hz.bins)} learned bins' if hz.bins else ''))
    d = np.where(dark)[0]
    if len(d) == 0: sys.exit(f'The Sun never reaches {a.sun_alt} deg tonight; try --sun-alt -12 (nautical)')
    print(f'Dark ({a.sun_alt:g} deg): {hhmm(times[d[0]])} to {hhmm(times[d[-1]])}. Moon {illum[d[0]]*100:.0f}% lit, ' +
          ('below the horizon all night' if (moon[d] < 0).all() else 'above the horizon all night' if (moon[d] > 0).all() else
           'up during ' + ', '.join(f'{hhmm(times[i])}' for i in d if moon[i] > 0 and (i == d[0] or moon[i - 1] <= 0)) + ' onward (rises/sets inside the dark period)'))

    rows = []
    for tg in load_targets():
        c = SkyCoord(tg['ra'] * u.hourangle, tg['dec'] * u.deg).transform_to(frame)
        alt, az = c.alt.deg, c.az.deg
        sep = SkyCoord(tg['ra'] * u.hourangle, tg['dec'] * u.deg).separation(moon_c).deg
        ok = dark & (alt > hz.min_alt(az)) & ((sep > a.moon_sep) | (moon < -2))
        if not ok.any(): continue
        # longest contiguous run
        best, cur, s = (0, 0, 0), 0, 0
        for i, v in enumerate(ok):
            if v:
                if cur == 0: s = i
                cur += 1
                if cur > best[0]: best = (cur, s, i)
            else: cur = 0
        n, i0, i1 = best
        if n * a.step < a.min_block: continue
        peak = i0 + int(np.argmax(np.where(ok[i0:i1 + 1], alt[i0:i1 + 1], -99)))
        rows.append(dict(tg, ok=ok, alt=alt, az=az, i0=i0, i1=i1, peak=peak, mins=n * a.step, score=tg['prio'] * min(1, n * a.step / 180) * (0.6 + 0.4 * min(1, alt[peak] / 60))))
    rows.sort(key=lambda r: -r['score'])

    def cardinal(z): return 'N NE E SE S SW W NW'.split()[int(((z + 22.5) % 360) // 45)]
    print(f'\n{"target":38} {"pri":>3} {"window":>13} {"mins":>5} {"peak":>6} {"alt":>4} {"az":>9}')
    for r in rows[:a.top]:
        p = r['peak']; print(f'{r["name"][:38]:38} {r["prio"]:3d} {hhmm(times[r["i0"]])}-{hhmm(times[r["i1"]])} {r["mins"]:5.0f} {hhmm(times[p]):>6} {r["alt"][p]:4.0f} {r["az"][p]:4.0f} {cardinal(r["az"][p]):>3}')

    # greedy plan: each target used once; take the best-scoring observable one that stays up >= min_block, favouring ones about to set,
    # and move on after max_block (the next best then takes over)
    print('\nPlan:'); i = d[0]; plan = []; used = set(); need = int(a.min_block / a.step); cap = int(a.max_block / a.step)
    while i <= d[-1]:
        best = None
        for r in rows:
            if r['key'] in used or not r['ok'][i]: continue
            run = 0
            while i + run < len(times) and r['ok'][i + run]: run += 1
            if run < need: continue
            v = r['prio'] * (0.5 + 0.5 * min(1, r['alt'][i:i + min(run, cap)].mean() / 60)) + (2 if run <= 1.5 * cap else 0)
            if best is None or v > best[0]: best = (v, r, run)
        if best is None: i += 1; continue
        r = best[1]; n = min(best[2], cap); plan.append((i, i + n - 1, r)); used.add(r['key']); i += n
    if not plan: print('  nothing observable for a useful block tonight')
    for s, e, r in plan:
        print(f'  {hhmm(times[s])}-{hhmm(times[e])}  {r["name"]:36} alt {r["alt"][s]:2.0f}->{r["alt"][e]:2.0f}, az {r["az"][s]:3.0f}->{r["az"][e]:3.0f}  ({(e - s + 1) * a.step:.0f} min)')

    if a.json:
        iso = lambda i: times[i].astimezone(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        info = lambda r: dict(name=r['name'], key=r['key'], ra_hours=r['ra'], dec_deg=r['dec'], priority=r['prio'], size_arcmin=r['size'], window_start=iso(r['i0']), window_end=iso(r['i1']),
                              peak=iso(r['peak']), peak_alt=round(float(r['alt'][r['peak']]), 1))
        json.dump(dict(night=str(day), dark_start=iso(d[0]), dark_end=iso(d[-1]), moon_illumination=round(float(illum[d[0]]), 2), horizon=src,
                       plan=[dict(info(r), start=iso(s), end=iso(e)) for s, e, r in plan], targets=[info(r) for r in rows]), real, indent=1)
        real.write('\n')

if __name__ == '__main__': main()
