#!/usr/bin/env python3
"""Multi-night stacking review of an Origin imaging archive, from the CSV that tools/archive_qc.py writes.

    tools/archive_report.py [--csv .../frames.csv] [--out .../archive_qc]

Per frame the acceptance rule mirrors the live analyser (SkyMonitor/Sequencer) as closely as a stand-alone frame allows:
  * not trailed        coherent streaks: at least 25 of the 100 brightest sources elongated the same way (frame-stats `streaks.trailed`)
  * not soft/defocused median star radius at most 2.6 half-resolution pixels (the live `defocused` threshold)
  * enough stars       at least 25, and at least half of the session's 90th percentile (a transparency proxy: cloud and haze remove stars)
  * sky not lifted     (median - black level) per second at most 1.5 x the session median (cloud, twilight, Moon)
  * not saturated      saturated fraction at most 1 %
Per session: frames, accepted, accepted exposure. Per target (sessions grouped by sky position): nights, accepted exposure per night and in total, rotation and pointing spread,
ISO and exposure mix. Writes sessions.csv, targets.csv and report.md.
"""
import argparse, csv, collections, math, os
import numpy as np

BLACK = 4985.0

def num(x):
    try: return float(x)
    except Exception: return float('nan')

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--csv', default='/mnt/fits-backup/claude/archive_qc/frames.csv'); ap.add_argument('--out', default='/mnt/fits-backup/claude/archive_qc'); a = ap.parse_args()
    rows = list(csv.DictReader(open(a.csv))); seen = {}; dup = collections.Counter(); keep = []
    for r in rows:                                                   # the same frame can sit in two folders (the M1 folder is a subset of M1_Crab_Nebula_2026-01-23): keep the longer session's copy
        k = (r['date_obs'], r['ra'], r['dec'])
        if k in seen: dup[r['session']] += 1; continue
        seen[k] = 1; keep.append(r)
    rows = keep
    by = collections.OrderedDict()
    for r in rows: by.setdefault(r['session'], []).append(r)
    sess = []
    for s, rs in by.items():
        st = np.array([num(r['stars']) for r in rs]); rad = np.array([num(r['star_radius_px']) for r in rs]); ex = np.array([num(r['exptime']) for r in rs])
        sky = (np.array([num(r['median_adu']) for r in rs]) - BLACK) / ex; sat = np.array([num(r['saturated_fraction']) for r in rs])
        trailed = np.array([r['trailed'] == 'True' for r in rs]); err = np.array([bool(r['error']) for r in rs])
        ok_sky = np.nanmedian(sky); p90 = np.nanpercentile(st, 90)
        why = {'error': err, 'trailed': trailed & ~err, 'soft': (rad > 2.6) & ~err, 'few_stars': ((st < 25) | (st < 0.5 * p90)) & ~err, 'sky_lifted': (sky > 1.5 * ok_sky) & ~err, 'saturated': (sat > 0.01) & ~err}
        bad = np.zeros(len(rs), bool)
        for v in why.values(): bad |= v
        good = ~bad
        t0 = rs[0]['date_obs'][:16]
        ra = np.nanmean([num(r['ra']) for r in rs]); dec = np.nanmean([num(r['dec']) for r in rs]); rot = np.array([num(r['rot_deg']) for r in rs]); rc = np.deg2rad(rot[~np.isnan(rot)])
        rmean = float(np.rad2deg(np.arctan2(np.sin(rc).mean(), np.cos(rc).mean())) % 360) if len(rc) else float('nan'); rdev = ((rot - rmean + 180) % 360) - 180
        sess.append(dict(session=s, night=t0[:10], start=t0, frames=len(rs), accepted=int(good.sum()), accepted_pct=100 * good.mean(), exp_s=float(np.nanmedian(ex)), iso=rs[0]['iso'],
                         accepted_exposure_s=float(ex[good].sum()), temp=num(rs[0]['ccd_temp']), ra=ra, dec=dec, rot_mean=rmean, rot_dev_min=float(np.nanmin(rdev)), rot_dev_max=float(np.nanmax(rdev)), centre_ok=int((~trailed | (rad <= 2.6)).sum() if False else (~(why['error'] | why['soft'] | why['few_stars'] | why['sky_lifted'] | why['saturated'])).sum()),
                         stars_med=float(np.nanmedian(st)), radius_med=float(np.nanmedian(rad)), sky_rate=float(ok_sky), **{'rej_' + k: int(v.sum()) for k, v in why.items()}))
    with open(os.path.join(a.out, 'sessions.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, list(sess[0].keys())); w.writeheader(); w.writerows(sess)
    # group sessions by sky position (within 0.6 degrees)
    groups = []
    for s in sess:
        for g in groups:
            d = math.hypot((s['ra'] - g[0]['ra']) * math.cos(math.radians(s['dec'])), s['dec'] - g[0]['dec'])
            if d < 0.6: g.append(s); break
        else: groups.append([s])
    tg = []
    for g in groups:
        nights = sorted({s['night'] for s in g}); per = collections.OrderedDict((n, sum(s['accepted_exposure_s'] for s in g if s['night'] == n)) for n in nights)
        tot = sum(per.values()); best = max(per.values()) if per else 0
        means = [s['rot_mean'] for s in g]; rspan = max(abs(((m - means[0] + 180) % 360) - 180) for m in means) + 0.0
        offs = max(math.hypot((s['ra'] - g[0]['ra']) * math.cos(math.radians(s['dec'])), s['dec'] - g[0]['dec']) for s in g) * 60
        tg.append(dict(target=g[0]['session'].split('_20')[0], sessions=len(g), nights=len(nights), frames=sum(s['frames'] for s in g), accepted=sum(s['accepted'] for s in g),
                       accepted_h=tot / 3600, best_night_h=best / 3600, snr_gain=math.sqrt(tot / best) if best else 0, iso=sorted({s['iso'] for s in g}), exps=sorted({s['exp_s'] for s in g}),
                       rot_span_between_sessions=rspan, accepted_ignoring_trailing_h=sum(s['centre_ok'] * s['exp_s'] for s in g) / 3600, pointing_spread_arcmin=offs, per_night={n: round(v / 3600, 2) for n, v in per.items()}))
    with open(os.path.join(a.out, 'targets.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, [k for k in tg[0].keys()]); w.writeheader(); w.writerows(tg)
    L = ['# Origin archive: multi-night stacking review', '', 'Frames scored: %d unique in %d sessions (%d duplicate copies of frames in a second folder ignored: %s). Acceptance rule: see the header of tools/archive_report.py.' % (len(rows), len(sess), sum(dup.values()), ', '.join('%s %d' % (k[:30], v) for k, v in dup.items()) or 'none'), '',
         '## Sessions', '', '| session | start | frames | accepted | % | exp s | ISO | stars | radius px | trailed | soft | few stars | sky lifted |', '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for s in sess:
        L.append('| %s | %s | %d | %d | %.0f | %.0f | %s | %.0f | %.2f | %d | %d | %d | %d |' % (s['session'][:40], s['start'], s['frames'], s['accepted'], s['accepted_pct'], s['exp_s'], s['iso'], s['stars_med'], s['radius_med'],
                                                                                    s['rej_trailed'], s['rej_soft'], s['rej_few_stars'], s['rej_sky_lifted']))
    L += ['', '## Targets (sessions grouped by sky position)', '', '| target | nights | frames | accepted | accepted h | if trailing is tolerated h | best night h | SNR gain vs best night | rotation between sessions deg | pointing spread arcmin | ISO | exposures s |', '|---|---|---|---|---|---|---|---|---|---|---|---|']
    for t in sorted(tg, key=lambda t: -t['accepted_h']):
        L.append('| %s | %d | %d | %d | %.2f | %.2f | %.2f | %.2fx | %.0f | %.1f | %s | %s |' % (t['target'], t['nights'], t['frames'], t['accepted'], t['accepted_h'], t['accepted_ignoring_trailing_h'], t['best_night_h'], t['snr_gain'], t['rot_span_between_sessions'], t['pointing_spread_arcmin'], '/'.join(t['iso']), '/'.join('%g' % e for e in t['exps'])))
    open(os.path.join(a.out, 'report.md'), 'w').write('\n'.join(L) + '\n'); print('\n'.join(L))

if __name__ == '__main__':
    main()
