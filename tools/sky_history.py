#!/usr/bin/env python3
"""Join the all-sky camera archive with what the telescope saw and what was forecast, one row per archived all-sky frame, to calibrate the camera's cloud estimate.

    tools/sky_history.py [--data /mnt/fits-backup/claude] [--out sky_history.csv]

Per all-sky frame (allsky/YYYYMMDD_HHMM.jpg, UTC): sun altitude; grey-level features of the central 75% of the fisheye (median, 20th/80th percentile, spread); the current
percentile-method cover estimate; the telescope's own frames within +-6 minutes (focus_log.csv: how many, fraction judged clear/hazy/patchy/cloudy, median sky rate and stars);
the archived dusk forecast for that hour (forecasts/<night>.json: cloud, low/mid/high, quality); and any label given with `sky-report` (allsky_labelled/labels.jsonl).
The all-sky archive is now kept for 400 days (it was 3), and this table grows into the calibration set: telescope verdicts are ground truth at night, labels by day.
"""
import argparse, csv, glob, json, math, os, collections
from datetime import datetime, timedelta, timezone
import numpy as np
from PIL import Image

def sun_alt(t, lat=52.2054, lon=0.1218):
    n = t.timestamp() / 86400 + 2440587.5 - 2451545.0; w = lambda d: d % 360
    L = w(280.460 + 0.9856474 * n); g = math.radians(w(357.528 + 0.9856003 * n)); lam = math.radians(L + 1.915 * math.sin(g) + 0.02 * math.sin(2 * g)); eps = math.radians(23.439)
    ra = math.atan2(math.cos(eps) * math.sin(lam), math.cos(lam)); dec = math.asin(math.sin(eps) * math.sin(lam)); gm = w(280.46061837 + 360.98564736629 * n)
    H = math.radians(gm + lon) - ra; la = math.radians(lat)
    return math.degrees(math.asin(math.sin(dec) * math.sin(la) + math.cos(dec) * math.cos(la) * math.cos(H)))

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--data', default='/mnt/fits-backup/claude'); ap.add_argument('--out', default='sky_history.csv'); a = ap.parse_args()
    UTC = timezone.utc
    tel = []
    for r in csv.DictReader(open(os.path.join(a.data, 'focus_log.csv'))):
        try: tel.append((datetime.fromisoformat(r['time'].replace('Z', '+00:00')), r['verdict'], float(r['sky_rate'] or 0), float(r['stars'] or 0)))
        except Exception: pass
    tel.sort(key=lambda x: x[0]); tt = [x[0] for x in tel]
    fc = {}
    for f in glob.glob(os.path.join(a.data, 'forecasts', '*.json')):
        for h in json.load(open(f)).get('hours', []):
            u = h.get('utc') or h.get('time')
            if u: fc[datetime.fromisoformat(u.replace('Z', '+00:00')).replace(minute=0, second=0, microsecond=0)] = h
    labels = []
    lf = os.path.join(a.data, 'allsky_labelled', 'labels.jsonl')
    if os.path.exists(lf):
        for l in open(lf):
            try: j = json.loads(l); labels.append((datetime.fromisoformat(j['time'].replace('Z', '+00:00')), j['cover_pct']))
            except Exception: pass
    yy, xx = np.mgrid[0:720, 0:576]; mask = np.hypot(xx - 288, yy - 360) < 0.75 * 288
    rows = []
    for f in sorted(glob.glob(os.path.join(a.data, 'allsky', '*.jpg'))):
        t = datetime.strptime(os.path.basename(f)[:13], '%Y%m%d_%H%M').replace(tzinfo=UTC)
        v = np.asarray(Image.open(f).convert('L')).astype(float)[mask]; p20, med, p80 = np.percentile(v, [20, 50, 80])
        near = [x for x in tel if abs((x[0] - t).total_seconds()) <= 360]
        c = collections.Counter(x[1] for x in near); n = len(near)
        fh = fc.get(t.replace(minute=0, second=0, microsecond=0), {})
        lab = [cv for (lt, cv) in labels if abs((lt - t).total_seconds()) <= 900]
        rows.append(dict(time=t.strftime('%Y-%m-%dT%H:%MZ'), sun_alt=round(sun_alt(t), 1), allsky_median=round(med), allsky_p20=round(p20), allsky_p80=round(p80), allsky_std=round(float(v.std()), 1),
                         method_cover=round(float((v > min(p20, 90) + 25).mean()), 2), tel_frames=n, tel_clear=round(sum(c[k] for k in ('clear', 'hazy')) / n, 2) if n else '', tel_cloudy=round(sum(c[k] for k in ('cloudy', 'unknown')) / n, 2) if n else '',
                         tel_sky_rate=round(float(np.median([x[2] for x in near])), 1) if n else '', tel_stars=round(float(np.median([x[3] for x in near]))) if n else '',
                         fc_cloud=round(fh.get('cloud', float('nan')), 0) if fh else '', fc_quality=round(fh.get('quality', float('nan')), 2) if fh else '', label_cover=lab[0] if lab else ''))
    with open(a.out, 'w', newline='') as fo:
        w = csv.DictWriter(fo, list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    both = [r for r in rows if r['tel_frames'] and r['sun_alt'] < -12]
    print('%d all-sky frames, %d with telescope frames within 6 min at night (Sun below -12)' % (len(rows), len(both)))
    if both:
        ok = [r for r in both if r['tel_clear'] >= 0.7]; bad = [r for r in both if r['tel_cloudy'] >= 0.7]
        for nm, g in (('telescope says clear', ok), ('telescope says cloud', bad)):
            if g: print('  %-22s n=%3d  all-sky median %s  (min %d, max %d)   fc cloud %s' % (nm, len(g), np.round(np.percentile([r['allsky_median'] for r in g], [10, 50, 90])), min(r['allsky_median'] for r in g), max(r['allsky_median'] for r in g),
                                                                                np.round(np.nanmedian([float(r['fc_cloud']) if r['fc_cloud'] != '' else np.nan for r in g]))))
        if ok and bad:
            best = max(range(40, 200, 2), key=lambda th: sum(r['allsky_median'] < th for r in ok) + sum(r['allsky_median'] >= th for r in bad)); acc = (sum(r['allsky_median'] < best for r in ok) + sum(r['allsky_median'] >= best for r in bad)) / (len(ok) + len(bad))
            print('  a single threshold on the all-sky median at night: %d grey levels separates the two with %.0f%% agreement' % (best, 100 * acc))
    print('wrote', a.out)

if __name__ == '__main__':
    main()
