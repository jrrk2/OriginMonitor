#!/usr/bin/env python3
"""Focus (HFR) against temperature over a night: chart + per-target statistics.

    tools/focus_vs_temperature.py --logs '~/Documents/CelestronOriginLogs/websocket_log_2026100[9]_*.txt' ... --seq '/data/sequence_*.jsonl' --out /data/focus_vs_temperature

Sources: Environment/GetStatus (ambient, front cell, camera temperature about once a second) and Focuser/GetStatus (position) from the telescope message logs,
and per-frame HFR from the sequencer JSON lines. Frames are binned to 5 minutes (median); focus blocks are the intervals between autofocus runs (one per target).
Within a block the focus position is fixed, so HFR against temperature there is the clean test. Seeing and altitude also move HFR, so correlations are weak evidence.
"""
import argparse, collections, datetime as dt, glob, json, os, re, statistics as st
import numpy as np
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt

INK, INK2, GRID, SURF = '#0b0b0b', '#52514e', '#e4e3df', '#fcfcfb'
COL = ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4']

def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--logs', nargs='+', required=True); ap.add_argument('--seq', nargs='+', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--from', dest='t0', default='2026-10-09T19:45:00'); ap.add_argument('--blocks', help='JSON list of [name, start ISO, end ISO]', required=True)
    return ap.parse_args()

def load_env(files, t0):
    env = collections.defaultdict(lambda: collections.defaultdict(list)); foc = {}
    pat = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+\] RECV: (\{.*\})$')
    for f in files:
        for line in open(f, errors='replace'):
            if '"Source":"Environment"' not in line and '"Source":"Focuser"' not in line: continue
            m = pat.match(line.strip())
            if not m: continue
            try: o = json.loads(m.group(2))
            except Exception: continue
            t = dt.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S')
            if t < t0: continue
            k = t.replace(second=0)
            if o.get('Source') == 'Environment' and 'AmbientTemperature' in o:
                for key in ('AmbientTemperature', 'FrontCellTemperature', 'CameraTemperature'): env[k][key].append(o[key])
            elif o.get('Source') == 'Focuser' and 'Position' in o: foc[k] = o['Position']
    return {k: {a: st.median(v) for a, v in d.items()} for k, d in env.items()}, foc

def main():
    a = parse_args(); t0 = dt.datetime.fromisoformat(a.t0)
    files = sorted(sum((glob.glob(os.path.expanduser(p)) for p in a.logs), []))
    env, foc = load_env(files, t0)
    frames = []
    for p in a.seq:
        for f in sorted(glob.glob(os.path.expanduser(p))):
            for l in open(f):
                r = json.loads(l)
                if r['stars'] > 0 and r['verdict'] in ('clear', 'hazy') and r['hfr_px'] > 0: frames.append((dt.datetime.fromisoformat(r['time'][:19]), r['hfr_px']))
    blocks = [(n, dt.datetime.fromisoformat(s), dt.datetime.fromisoformat(e)) for n, s, e in json.loads(a.blocks)]
    ts = sorted(env); T = lambda key: np.array([env[t][key] for t in ts])
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'axes.edgecolor': INK2, 'axes.labelcolor': INK2, 'xtick.color': INK2, 'ytick.color': INK2, 'text.color': INK})
    fig, ax = plt.subplots(3, 1, figsize=(12, 9), sharex=True, gridspec_kw={'height_ratios': [3, 2, 1.4]}); fig.patch.set_facecolor(SURF)
    for x in ax: x.set_facecolor(SURF); x.grid(True, color=GRID, lw=0.8); [s.set_visible(False) for k, s in x.spines.items() if k in ('top', 'right')]
    stats = []; binned = []
    for i, (n, s, e) in enumerate(blocks):
        pts = [(t, h) for t, h in frames if s <= t < e]
        if not pts: continue
        ax[0].plot([t for t, h in pts], [h for t, h in pts], '.', ms=2.5, color=COL[i], alpha=0.25, mew=0)
        b = collections.defaultdict(list)
        for t, h in pts: b[t.replace(minute=t.minute // 5 * 5, second=0)].append(h)
        bt = sorted(b); bh = [st.median(b[k]) for k in bt]
        ax[0].plot(bt, bh, '-', color=COL[i], lw=1.8); ax[0].axvspan(s, e, color=COL[i], alpha=0.05, lw=0)
        pos = [foc[t] for t in sorted(foc) if s <= t < e]
        ax[0].text(s + (e - s) / 2, 3.72, f'{n}\nfocus {int(st.median(pos))}' if pos else n, ha='center', va='top', fontsize=9, color=INK)
        row = {'block': n, 'frames': len(pts), 'minutes': round((e - s).total_seconds() / 60), 'hfr_med': round(st.median([h for t, h in pts]), 2), 'focus': int(st.median(pos)) if pos else None}
        for key, lab in (('AmbientTemperature', 'ambient'), ('FrontCellTemperature', 'front'), ('CameraTemperature', 'camera')):
            tv = [env[k][key] for k in bt if k in env]; hv = [h for k, h in zip(bt, bh) if k in env]
            row[lab + '_range'] = [round(min(tv), 1), round(max(tv), 1)] if tv else None
            if len(tv) >= 6 and np.ptp(tv) > 0.3:
                sl, ic = np.polyfit(tv, hv, 1); r = np.corrcoef(tv, hv)[0, 1]
                bs = []; rng = np.random.default_rng(1)
                for _ in range(500):
                    ix = rng.integers(0, len(tv), len(tv))
                    if np.ptp(np.array(tv)[ix]) > 0: bs.append(np.polyfit(np.array(tv)[ix], np.array(hv)[ix], 1)[0])
                row[lab + '_slope_px_per_C'] = [round(sl, 3), round(np.percentile(bs, 2.5), 3), round(np.percentile(bs, 97.5), 3)]; row[lab + '_r'] = round(r, 2)
            binned += [(n, k.isoformat(), h, env[k]['AmbientTemperature'], env[k]['FrontCellTemperature'], env[k]['CameraTemperature']) for k, h in zip(bt, bh) if k in env]
        stats.append(row)
    ax[0].set_ylabel('HFR (pixels, 1.48 arcsec each)\nlower is sharper'); ax[0].set_ylim(1.6, 3.8)
    for key, lab, ls, col in (('FrontCellTemperature', 'front cell (dew-heated)', '-', INK), ('AmbientTemperature', 'ambient', '--', INK2), ('CameraTemperature', 'camera', ':', INK2)):
        ax[1].plot(ts, T(key), ls, color=col, lw=1.6); j = int(len(ts) * 0.93); ax[1].text(ts[j], T(key)[j] + 0.6, lab, color=col, fontsize=9, ha='right')
    ax[1].set_ylabel('temperature (deg C)')
    fts = sorted(foc); ax[2].step(fts, [foc[t] for t in fts], where='post', color=INK2, lw=1.4); ax[2].set_ylabel('focuser position\n(steps)'); ax[2].set_ylim(18850, 19050)
    ax[2].set_xlabel('local time (BST), 2026-10-09 to 10-10')
    import matplotlib.dates as md; ax[2].xaxis.set_major_formatter(md.DateFormatter('%H:%M')); ax[2].xaxis.set_major_locator(md.HourLocator(interval=1)); ax[2].set_xlim(blocks[0][1] - dt.timedelta(minutes=10), blocks[-1][2] + dt.timedelta(minutes=10))
    fig.suptitle('Focus against temperature, firmware autofocus only (no manual focusing)', x=0.01, ha='left', fontsize=13, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.97)); fig.savefig(a.out + '.png', dpi=130, facecolor=SURF)
    fig2, x2 = plt.subplots(figsize=(7.5, 5)); fig2.patch.set_facecolor(SURF); x2.set_facecolor(SURF); x2.grid(True, color=GRID, lw=0.8)
    names = [b[0] for b in blocks]
    for n in names:
        pts = [(r[3], r[2]) for r in binned if r[0] == n]
        if pts: x2.plot(*zip(*pts), 'o', ms=5, color=COL[names.index(n)], mec=SURF, mew=1, label=n)
    x2.set_xlabel('ambient temperature (deg C)'); x2.set_ylabel('HFR, 5-minute median (pixels)'); x2.legend(frameon=False, fontsize=9); [s.set_visible(False) for k, s in x2.spines.items() if k in ('top', 'right')]
    fig2.tight_layout(); fig2.savefig(a.out + '_scatter.png', dpi=130, facecolor=SURF)
    with open(a.out + '.csv', 'w') as f:
        f.write('block,time,hfr_px_5min_median,ambient_C,front_cell_C,camera_C\n'); [f.write(','.join(map(str, r)) + '\n') for r in binned]
    json.dump(stats, open(a.out + '_stats.json', 'w'), indent=1); print(json.dumps(stats, indent=1))

if __name__ == '__main__': main()
