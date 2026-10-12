#!/usr/bin/env python3
"""Figures for the CAA Capella article. Reads the data directory (frame logs, stacks, forecast record); writes figs/*.png."""
import csv, glob, json, collections, os, shutil
from datetime import datetime, timedelta
import numpy as np
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt, matplotlib.dates as mdates
from PIL import Image
D = '/mnt/fits-backup/claude/'; OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'figs/')
BST = timedelta(hours=1)
COL = {'clear': '#2a9d8f', 'hazy': '#e9c46a', 'patchy': '#f4a261', 'cloudy': '#9aa5b1', 'unknown': '#6c757d', 'trailed': '#e76f51', 'defocused': '#9b5de5', 'unreadable': '#cccccc'}
plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False, 'figure.dpi': 150})
def t(s): return datetime.strptime(s[:19], '%Y-%m-%dT%H:%M:%S') + BST            # logs are UTC; plot in BST
NIGHTS = [('Night 1: 9/10 October', '2026-10-09T18:00:00', '2026-10-10T05:30:00'), ('Night 2: 11/12 October', '2026-10-11T18:00:00', '2026-10-12T05:30:00')]
rows = [r for r in csv.DictReader(open(D + 'focus_log.csv'))]
seq = []
for f in sorted(glob.glob(D + 'sequence_2026*.jsonl')):
    for l in open(f):
        try: seq.append(json.loads(l))
        except Exception: pass
def inwin(ts, a, b): return a <= ts[:19] < b

TCOL = {}
Z = 1 / 1.6                                              # figures are shown at page width: a smaller canvas makes text and symbols 60% bigger on the page
# ---- 1. timeline of both nights: sky level coloured by verdict, target bands, cumulative stacked exposure
fig, axs = plt.subplots(4, 1, figsize=(10 * Z, 11 * Z * 1.2), gridspec_kw={'height_ratios': [3, 1.2, 3, 1.2]})
for k, (title, a, b) in enumerate(NIGHTS):
    ax, ax2 = axs[2 * k], axs[2 * k + 1]
    rr = [r for r in rows if inwin(r['time'], a, b) and float(r['sky_rate'] or 0) > 0]
    for v in COL:
        xs = [t(r['time']) for r in rr if r['verdict'] == v]; ys = [float(r['sky_rate']) for r in rr if r['verdict'] == v]
        if xs: ax.scatter(xs, ys, s=7, c=COL[v], label='%s (%d)' % (v, len(xs)), lw=0)
    ax.set_yscale('log'); ax.set_ylabel('sky level (ADU/s, log)'); ax.set_title(title, loc='left', fontweight='bold')
    lo, hi = ax.get_ylim(); ax.set_ylim(lo, hi * 12)
    ax.legend(ncol=4, fontsize=6.5, frameon=False, loc='upper left', markerscale=1.6, columnspacing=1.0, handletextpad=0.2)
    # target bands
    segs = []; cur = None
    for r in sorted((r for r in rows if inwin(r['time'], a, b) and r['target']), key=lambda r: r['time']):
        tm = t(r['time'])
        if cur and cur[0] == r['target'] and (tm - cur[2]).total_seconds() < 900: cur[2] = tm            # same target, gap under 15 min: one block
        else: cur = [r['target'], tm, tm]; segs.append(cur)
    for name, s0, s1 in segs:
        mins = (s1 - s0).total_seconds() / 60
        if mins < 5: continue
        short = name.replace('NGC 869/884 Double Cluster', 'Double Cluster').replace('IC 1396 Elephant Trunk', 'IC 1396').replace('IC 5070 Pelican Nebula', 'Pelican').replace('NGC2174_Monkey_Head', 'Monkey Head').replace('NGC6992_Veil', 'Veil').replace('IC405_Flaming_Star', 'Flaming Star').replace('M31_centred', 'M31').replace('auto', 'probe')
        ax2.barh(0, mins / 1440, left=mdates.date2num(s0), height=0.6, color=TCOL.setdefault(short, plt.cm.tab20(len(TCOL) % 20)), alpha=0.85)
        if mins >= 45: ax2.text(mdates.date2num(s0) + mins / 2880, 0, short, ha='center', va='center', fontsize=6.5, clip_on=True)
        elif mins >= 12: ax2.text(mdates.date2num(s0) + mins / 2880, 0, short, ha='center', va='center', fontsize=6, rotation=90, clip_on=True)
    ax2.set_yticks([]); ax2.spines['left'].set_visible(False); ax2.set_ylabel('target')
    x0, x1 = t(a) + timedelta(hours=1), t(b)
    for x in (ax, ax2): x.set_xlim(x0, x1); x.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
ax.set_xlabel('local time (BST)'); axs[3].set_xlabel('local time (BST)')
fig.tight_layout(); fig.savefig(OUT + 'fig_timelines.png'); plt.close(fig)

# ---- 2. verdict mix per night and stacked exposure
fig, (a1, a2) = plt.subplots(1, 2, figsize=(10 * Z * 1.25, 3.6 * Z * 1.6))
stats = []
for title, a, b in NIGHTS:
    ss = [r for r in seq if inwin(r['time'], a, b)]
    c = collections.Counter(r['verdict'] for r in ss); n = sum(c.values())
    stk = sum(r['exposure'] for r in ss if r.get('stacked')); stats.append((title, n, c, stk, sum(1 for r in ss if r.get('stacked'))))
bottoms = np.zeros(2)
for v in ['clear', 'patchy', 'hazy', 'cloudy', 'unknown', 'trailed', 'defocused', 'unreadable']:
    h = np.array([100.0 * s[2].get(v, 0) / s[1] for s in stats]); a1.bar([0, 1], h, bottom=bottoms, color=COL[v], label=v); bottoms += h
a1.set_xticks([0, 1]); a1.set_xticklabels(['9/10 Oct\n%d frames' % stats[0][1], '11/12 Oct\n%d frames' % stats[1][1]]); a1.set_ylabel('% of frames'); a1.legend(fontsize=7, frameon=False, bbox_to_anchor=(1.0, 1.0)); a1.set_title('What the analyser made of each frame', loc='left')
a2.bar([0, 1], [s[3] / 3600 for s in stats], color=['#264653', '#2a9d8f']); [a2.text(i, s[3] / 3600 + 0.1, '%.1f h\n%d frames' % (s[3] / 3600, s[4]), ha='center', fontsize=9) for i, s in enumerate(stats)]
a2.set_xticks([0, 1]); a2.set_xticklabels(['9/10 Oct\nmostly clear', '11/12 Oct\nbroken cloud']); a2.set_ylabel('hours of exposure stacked'); a2.set_ylim(0, max(s[3] for s in stats) / 3600 * 1.3); a2.set_title('Data kept', loc='left')
fig.tight_layout(); fig.savefig(OUT + 'fig_verdicts.png'); plt.close(fig)
json.dump([{'night': s[0], 'frames': s[1], 'verdicts': dict(s[2]), 'stacked_s': s[3], 'stacked_frames': s[4]} for s in stats], open(OUT + 'night_stats.json', 'w'), indent=1)

# ---- 3. cloud and clear are separated by sky level
fig, ax = plt.subplots(figsize=(7 * Z * 1.3, 3.4 * Z * 1.3)); bins = np.logspace(1.5, 5.5, 60)
allr = [r for r in rows if float(r['sky_rate'] or 0) > 0 and r['verdict'] in ('clear', 'hazy', 'patchy', 'cloudy')]
for v in ('clear', 'patchy', 'hazy', 'cloudy'):
    ax.hist([float(r['sky_rate']) for r in allr if r['verdict'] == v], bins=bins, histtype='stepfilled', alpha=0.6, color=COL[v], label='%s (%d)' % (v, sum(1 for r in allr if r['verdict'] == v)))
ax.set_xscale('log'); ax.set_xlabel('sky level (ADU per second above black, log)'); ax.set_ylabel('frames'); ax.legend(frameon=False); ax.set_title('Sky level by verdict', loc='left')
fig.tight_layout(); fig.savefig(OUT + 'fig_skylevel.png'); plt.close(fig)

# ---- 4. forecast against what the telescope saw (night 2 only: the record started that night)
fc = [r for r in csv.DictReader(open(D + 'forecast_accuracy.csv'))]; fa = json.load(open(D + 'forecast_accuracy.json'))['summary']
fig, (a1, a2) = plt.subplots(1, 2, figsize=(10 * Z * 1.25, 3.6 * Z * 1.6), gridspec_kw={'width_ratios': [1.6, 1]})
x = [datetime.strptime(r['hour_utc'], '%Y-%m-%dT%H:%M:%SZ') + BST for r in fc]; w = 0.015
a1.bar(mdates.date2num(x) - w / 1.1, [float(r['forecast_quality']) for r in fc], width=w, color='#e9c46a', label='forecast quality (blend of 7 models)')
a1.bar(mdates.date2num(x) + w / 1.1, [float(r['observed_usable']) for r in fc], width=w, color='#264653', label='fraction of the hour with usable frames')
a1.xaxis_date(); a1.xaxis.set_major_locator(mdates.HourLocator(interval=2)); a1.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M')); a1.legend(fontsize=8, frameon=False, loc='upper left'); a1.set_ylim(0, 1.1); a1.set_title('Night 2: forecast against measurement', loc='left'); a1.set_xlabel('local time (BST)')
m = fa['by_model_best_first']; a2.barh([q['model'].replace('_seamless', '').replace('_single', '') for q in m][::-1], [q['mean_abs_error'] for q in m][::-1], color='#2a9d8f'); a2.set_xlabel('mean absolute error (0..1 quality)'); a2.set_title('By model (10 hours)', loc='left')
fig.tight_layout(); fig.savefig(OUT + 'fig_forecast.png'); plt.close(fig)

# ---- 5. gallery of every stack from both nights
G = [('M31 Andromeda', 'M31_centred', 'night 1'), ('NGC 6992 Veil', 'NGC6992_Veil_final_0109', 'night 1'), ('IC 405 Flaming Star', 'IC405_Flaming_Star_final_0325', 'night 1'), ('NGC 2174 Monkey Head', 'NGC2174_Monkey_Head_final_0510', 'night 1'),
     ('NGC 869/884 Double Cluster', 'NGC_869_884_Double_Cluster', 'night 2'), ('M1 Crab Nebula', 'M1', 'night 2'), ('M13 Hercules cluster', 'M13', 'night 2')]
import struct
fig, axs = plt.subplots(4, 2, figsize=(7.5, 9.2))
for ax, (name, d, nt) in zip(axs.flat, G):
    b = open(D + 'stacks/%s/state.bin' % d, 'rb').read(32); w_, h_, fr = struct.unpack('<iii', b[4:16]); ex = struct.unpack('<d', b[16:24])[0]
    ax.imshow(Image.open(D + 'stacks/%s/preview.png' % d).convert('RGB')); ax.set_xticks([]); ax.set_yticks([]); [s.set_visible(False) for s in ax.spines.values()]
    ax.set_title('%s\n%d frames, %s (%s)' % (name, fr, '%.1f h' % (ex / 3600) if ex >= 3600 else '%d min' % round(ex / 60), nt), fontsize=10)
axs.flat[7].axis('off')
fig.tight_layout(); fig.savefig(OUT + 'fig_gallery.png'); plt.close(fig)
shutil.copy(D + 'focus_vs_temperature.png', OUT + 'fig_focus_temperature.png')
print('done', stats[0][1:2], stats[1][1:2], [s[3] for s in stats])
