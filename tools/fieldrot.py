#!/usr/bin/env python3
"""Field rotation limit on exposure time for an alt-az mount (the Origin has no derotator).

    tools/fieldrot.py 20.9367 31.72                 # RA hours, Dec deg: table over tonight
    tools/fieldrot.py 0.7123 41.269 --tol 1.5

Rotation rate  w = Omega * cos(lat) * cos(az) / cos(alt)   (rad/s, Omega = sidereal rate), zero when the target is due E or W,
largest near the meridian at high altitude. A star at the frame corner (half-diagonal 1840 px of the 3056x2048 frame) trails
r*w*t pixels in an exposure of t seconds, so  t_max = tol_px / (r * |w|).  The centre does not trail at all.
Tracking errors are not included. A star is about 4.6 px FWHM here (HFR 2.3 px), so 2 px of trail at the very corner is hardly visible.
"""
import argparse, warnings
from datetime import datetime, timedelta, timezone
warnings.filterwarnings('ignore')
import numpy as np
import astropy.units as u
from astropy.coordinates import AltAz, EarthLocation, SkyCoord
from astropy.time import Time

SITE = EarthLocation(lat=52.2054 * u.deg, lon=0.1218 * u.deg, height=20 * u.m)
OMEGA = 7.2921159e-5            # rad/s
R_CORNER_PX = 1840.0            # half-diagonal of the full-resolution frame

def max_exposure(ra_hours, dec_deg, when=None, tol_px=2.0):
    """returns (t_max seconds, alt, az, rotation deg/hour) for one instant (UTC datetime or astropy Time, default now)"""
    t = Time(when) if when is not None else Time.now()
    c = SkyCoord(ra_hours * u.hourangle, dec_deg * u.deg).transform_to(AltAz(obstime=t, location=SITE))
    alt, az = c.alt.rad, c.az.rad
    w = OMEGA * np.cos(SITE.lat.rad) * np.cos(az) / np.cos(alt)
    tmax = np.inf if abs(w) < 1e-12 else tol_px / (R_CORNER_PX * abs(w))
    return float(tmax), float(np.degrees(alt)), float(np.degrees(az)), float(np.degrees(w) * 3600)

def worst_case(ra_hours, dec_deg, start, minutes, tol_px=2.0):
    """smallest t_max over a window (the exposure must be safe for the whole block)"""
    ts = [max_exposure(ra_hours, dec_deg, start + timedelta(minutes=m), tol_px)[0] for m in range(0, int(minutes) + 1, 5)]
    return min(ts)

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ra_hours', type=float); ap.add_argument('dec_deg', type=float); ap.add_argument('--tol', type=float, default=2.0, help='allowed corner trail, px')
    ap.add_argument('--hours', type=float, default=8); a = ap.parse_args()
    now = datetime.now(timezone.utc)
    print(f'RA {a.ra_hours} h  Dec {a.dec_deg:+} deg   corner trail tolerance {a.tol} px (1 px = 1.48 arcsec)')
    print('local   alt   az  rotation(deg/h)  t_max(s)  t_max at 1 px')
    for m in range(0, int(a.hours * 60) + 1, 30):
        t = now + timedelta(minutes=m); tm, alt, az, w = max_exposure(a.ra_hours, a.dec_deg, t, a.tol); t1 = max_exposure(a.ra_hours, a.dec_deg, t, 1.0)[0]
        print(f'{t.astimezone().strftime("%H:%M")}  {alt:4.0f} {az:4.0f}  {w:10.1f}      {tm:7.0f}   {t1:7.0f}' + ('' if alt > 25 else '   (low)'))
