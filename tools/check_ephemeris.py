#!/usr/bin/env python3
"""Sanity check only: compare OriginHeadless `body` (JPL DE440 via CSPICE, the reference) with astropy's built-in ephemeris.

    tools/check_ephemeris.py [--bin build/headless/build/OriginHeadless]

Compares altitude/azimuth (astropy's AltAz transform is geocentric-correct). Do not feed astropy's .icrs/FK5 coordinates of a planet to a goto: with a distance
attached they are barycentric and Jupiter came out 9 deg off. Anything over a degree here means one of the two is misconfigured.
"""
import argparse, json, subprocess, warnings
warnings.filterwarnings('ignore')
from astropy.coordinates import AltAz, EarthLocation, get_body
from astropy.time import Time
import astropy.units as u

ap = argparse.ArgumentParser(); ap.add_argument('--bin', default='build/headless/build/OriginHeadless'); ap.add_argument('--lat', type=float, default=52.2054); ap.add_argument('--lon', type=float, default=0.1218); a = ap.parse_args()
r = json.loads(subprocess.run([a.bin, 'body', '--lat', str(a.lat), '--lon', str(a.lon)], capture_output=True, text=True).stdout)
t = Time(r['time_utc'].replace('Z', ''), scale='utc'); loc = EarthLocation(lat=a.lat * u.deg, lon=a.lon * u.deg, height=20 * u.m)
print(r['ephemeris']); worst = 0
for b in r['bodies']:
    if b['name'] == 'pluto': continue                  # not in astropy's built-in ephemeris
    x = get_body(b['name'], t, loc).transform_to(AltAz(obstime=t, location=loc))
    dalt = x.alt.deg - b['alt_deg']; daz = (x.az.deg - b['az_deg'] + 180) % 360 - 180; sep = max(abs(dalt), abs(daz) * abs(__import__('math').cos(__import__('math').radians(b['alt_deg']))))
    worst = max(worst, sep if b['alt_deg'] > -5 else 0)
    print('%-8s alt %7.2f (astropy %7.2f)  az %7.2f (astropy %7.2f)  diff %.3f deg' % (b['name'], b['alt_deg'], x.alt.deg, b['az_deg'], x.az.deg, sep))
print('worst difference above the horizon: %.3f deg (%s)' % (worst, 'ok' if worst < 0.5 else 'CHECK'))
