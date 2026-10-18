#!/usr/bin/env python3
"""Sensor linearity and photon-transfer check on the twilight sky (no slew, no mount command: only exposure/ISO settings and snapshots).

    tools/linearity.py --out DIR [--isos 800,1600] [--cycles 6] [--every 120] [--keep]
    tools/linearity.py --analyse DIR/records.json

Twilight dims by roughly 10 % a minute, so a plain exposure ladder would drift. Every ladder point is two identical frames (A, B) and the points are bracketed by reference frames
at a fixed exposure; the reference level interpolated to the time of each pair removes the drift. For each ISO and Bayer colour (G, R, B), in a patch away from the frame centre:
  * linearity: pair mean / reference, against exposure / reference exposure; a straight line fitted over 5-70 % of full scale gives the black level, and the residuals of all points
    (percent) show compression near full scale and any floor at the shortest exposures
  * photon transfer: variance of (A - B)/sqrt 2 against signal gives the gain (e-/ADU) and the read noise (e-); that is what decides whether a higher ISO buys anything for short exposures
Each cycle re-measures the sky rate and adapts the ladder, so as the sky darkens the ladder reaches longer exposures. Frames are deleted after measuring unless --keep.
"""
import argparse, json, os, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, '..'))
FULL = 65535.0; BLACK0 = 4985.0
PLANES = {'G': [(0, 0), (1, 1)], 'R': [(0, 1)], 'B': [(1, 0)]}
Y0, Y1, X0, X1 = 150, 750, 150, 750          # patch (frame px), off the centre where the mount may be parked on Polaris


class Scope:
    """snapshots through the control server"""
    def __init__(self, port, outdir, keep):
        from originctl import call
        self.call, self.port, self.dir, self.keep = call, port, outdir, keep
    def grab(self, t, iso):
        r = self.call(self.port, 120, cmd='snapshot', dir=self.dir, exposure=float(t), iso=int(iso), binning=1, timeout=60)
        if not r.get('ok') or not r['path'].endswith('.fits'): raise RuntimeError('snapshot failed: %s' % r)
        from astropy.io import fits
        d = fits.getdata(r['path']).astype(np.float64)
        if not self.keep: os.remove(r['path'])
        return d


def plane_stats(frame, other=None):
    """clipped mean per colour of a frame in the patch; with a second frame of the same exposure also the variance of one frame from the difference"""
    out = {}
    for name, sites in PLANES.items():
        a = np.concatenate([frame[Y0 + dy:Y1:2, X0 + dx:X1:2].ravel() for dy, dx in sites]); m = a.mean()
        if other is None:
            med = np.median(a); mad = max(1.4826 * np.median(abs(a - med)), 1.0); k = abs(a - med) < 5 * mad; out[name] = {'mean': float(a[k].mean())}
        else:
            b = np.concatenate([other[Y0 + dy:Y1:2, X0 + dx:X1:2].ravel() for dy, dx in sites]); b = b * (a.mean() / b.mean())           # match the means: the sky moved a little between the two
            dff = (a - b) / np.sqrt(2); med = np.median(dff); mad = max(1.4826 * np.median(abs(dff - med)), 0.7); k = abs(dff - med) < 5 * mad
            out[name] = {'mean': float((a[k].mean() + b[k].mean()) / 2), 'var': float(dff[k].var()), 'n': int(k.sum())}
    return out


def run(scope, isos, cycles, every, log, updown=True):
    recs = []
    def save(): json.dump(recs, open(os.path.join(scope.dir, 'records.json'), 'w'))
    def take(kind, iso, t, partner=None):
        fa = scope.grab(t, iso); st = plane_stats(fa); rec = {'kind': kind, 'iso': iso, 't': t, 'time': time.time(), 'mean': {k: v['mean'] for k, v in st.items()}, 'sat': float((fa[Y0:Y1, X0:X1] >= FULL).mean())}
        recs.append(rec); save(); return fa, rec
    for cyc in range(cycles):
        for iso in isos:
            t = 1e-3                                                                                    # probe the sky rate and pick the reference exposure for ~25 % of full scale
            for _ in range(6):
                _, pr = take('probe', iso, t); s = pr['mean']['G'] - BLACK0
                if pr['sat'] > 0.001 or s > 0.6 * FULL: t /= 8
                elif s < 300: t = min(t * 8, 8.0)
                else: break
            rate = max(pr['mean']['G'] - BLACK0, 1.0) / t; tref = float(np.clip(15000.0 / rate, 1e-4, 6.0))
            cap = FULL - BLACK0                                                                         # targets: 97/93/86/78 % of full scale, then 70 % falling by half-octaves; brightest first, and each exposure
            targets = [f * cap for f in (0.97, 0.93, 0.86, 0.78)] + [0.7 * cap * 2 ** (-k / 2) for k in range(0, 23)]        # is recomputed from the latest reference so the dimming sky does not move the targets
            log('cycle %d ISO %d: sky %.0f ADU/s, reference %.4g s, %d ladder points from 97 %% of full scale down to %.0f ADU' % (cyc + 1, iso, rate, tref, len(targets), targets[-1]))
            _, lastref = take('ref', iso, tref)
            for direction, seq in (('down', targets), ('up', targets[::-1])) if updown else (('down', targets),):     # down: bright to faint; up: back from faint to bright. Any difference at the same signal is a memory effect
                n = 0
                for target in seq:
                    rate_now = max(lastref['mean']['G'] - BLACK0, 1.0) / tref; tk = target / rate_now
                    if not 2e-5 <= tk <= 30: continue
                    a_, ra = take('a', iso, tk); b_ = scope.grab(tk, iso); st = plane_stats(a_, b_)
                    recs.append({'kind': 'pair', 'dir': direction, 'target': float(target), 'iso': iso, 't': tk, 'tref': tref, 'time': (ra['time'] + time.time()) / 2, 'mean': {c: v['mean'] for c, v in st.items()}, 'var': {c: v['var'] for c, v in st.items()}, 'sat': ra['sat']}); n += 1
                    if n % 2 == 0: _, lastref = take('ref', iso, tref)
                _, lastref = take('ref', iso, tref)
        if cyc < cycles - 1: log('waiting %d s' % every); time.sleep(every)
    return recs


def analyse(recs, log=print):
    res = {}
    for iso in sorted({r['iso'] for r in recs}):
        R = [r for r in recs if r['iso'] == iso]; res[str(iso)] = {}; refs = [r for r in R if r['kind'] == 'ref']
        for c in PLANES:
            rows = []; dirs = []
            for p in [r for r in R if r['kind'] == 'pair']:
                mine = [r for r in refs if abs(r['t'] - p['tref']) < 1e-12 and abs(r['time'] - p['time']) < 90]
                before = [r for r in mine if r['time'] <= p['time']]; after = [r for r in mine if r['time'] >= p['time']]
                if not before or not after: continue
                b_, a_ = before[-1], after[0]; w = 0.0 if a_['time'] == b_['time'] else (p['time'] - b_['time']) / (a_['time'] - b_['time'])
                rows.append((p['t'], p['tref'], p['mean'][c], b_['mean'][c] * (1 - w) + a_['mean'][c] * w, p['var'][c], p['sat'])); dirs.append(p.get('dir', 'down'))
            if len(rows) < 8: res[str(iso)][c] = {'error': 'too few points (%d)' % len(rows)}; continue
            t, tref, mean, ref, var, sat = np.array(rows).T; dirs = np.array(dirs); cap = FULL - BLACK0
            def rho(b): return (mean - b) / (ref - b) * tref / t              # 1 for every point if the response is linear (drift removed by the bracketing references)
            def window(b, lo, hi): fs = (mean - b) / cap; return (fs > lo) & (fs < hi) & (sat == 0)
            best = None
            for b in np.arange(BLACK0 - 300, BLACK0 + 300, 1.0):                  # black level: the one for which the faintest points agree with the mid-range ones
                w = window(b, 0.1, 0.5); low = (mean - b > 0) & ((mean - b) / cap < 0.02)
                if w.sum() < 3 or low.sum() < 3: continue
                r = rho(b); score = float(np.sum((r[low] / np.median(r[w]) - 1) ** 2))
                if best is None or score < best[0]: best = (score, b)
            black = best[1] if best else BLACK0; r = rho(black); lin = window(black, 0.05, 0.70)
            if lin.sum() < 4: res[str(iso)][c] = {'error': 'too few points in the 5-70%% window (%d)' % lin.sum()}; continue
            resid = 100 * (r / np.median(r[lin]) - 1); fs = (mean - black) / cap; S = mean - black
            ok = (S > 0.02 * cap) & (S < 0.6 * cap) & (sat == 0)
            c1, c0 = np.polyfit(S[ok], var[ok], 1) if ok.sum() >= 4 else (np.nan, np.nan); gain = 1 / c1 if c1 > 0 else np.nan; rn = float(np.sqrt(max(c0, 0)))
            ud = None
            if (dirs == 'up').any() and (dirs == 'down').any():                    # pair each upward point with the downward point of the same target and compare the deviations
                dn = np.nonzero(dirs == 'down')[0]; up = np.nonzero(dirs == 'up')[0]; diffs = []
                for i in up:
                    j = dn[np.argmin(abs(fs[dn] - fs[i]) / np.maximum(fs[i], 1e-6))]
                    if abs(fs[j] / fs[i] - 1) < 0.15 and fs[i] > 0.003: diffs.append((fs[i], resid[i] - resid[j]))
                if diffs:
                    D = np.array(diffs); bands = ((0.003, 0.05), (0.05, 0.3), (0.3, 0.7), (0.7, 1.0))
                    ud = {'matched': int(len(D)), 'mean_up_minus_down_pct': float(D[:, 1].mean()), 'max_abs_pct': float(np.abs(D[:, 1]).max()),
                          'by_band_pct': {'%g-%g' % b_: (float(D[(D[:, 0] >= b_[0]) & (D[:, 0] < b_[1]), 1].mean()) if ((D[:, 0] >= b_[0]) & (D[:, 0] < b_[1])).any() else None) for b_ in bands}}
            res[str(iso)][c] = {'up_down': ud, 'points': int(len(rows)), 'black_level_adu': float(black), 'max_abs_residual_pct_5_70': float(np.abs(resid[lin]).max()), 'rms_residual_pct_5_70': float(np.sqrt((resid[lin] ** 2).mean())),
                                'residual_pct_above_70': [[float(a_), float(b_)] for a_, b_ in zip(fs[fs >= 0.70], resid[fs >= 0.70])], 'residual_pct_below_2pct_of_scale': [[float(a_), float(b_)] for a_, b_ in zip(fs[(fs < 0.02) & (fs > 0)], resid[(fs < 0.02) & (fs > 0)])],
                                'gain_e_per_adu': float(gain), 'read_noise_adu': rn, 'read_noise_e': float(rn * gain), 'curve': {'signal_fraction': fs.tolist(), 'residual_pct': resid.tolist(), 'var': var.tolist(), 'S': S.tolist(), 'dir': dirs.tolist()}}
    return res


def plot(res, path):
    import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
    isos = list(res); fig, ax = plt.subplots(2, len(isos), figsize=(6 * len(isos), 8), squeeze=False)
    for j, iso in enumerate(isos):
        for c, col in (('G', 'g'), ('R', 'r'), ('B', 'b')):
            d = res[iso].get(c)
            if not d or 'curve' not in d: continue
            cv = d['curve']; fsa = np.array(cv['signal_fraction']).clip(1e-4); rsa = np.array(cv['residual_pct']); da = np.array(cv.get('dir', ['down'] * len(fsa)))
            ax[0, j].semilogx(fsa[da == 'down'], rsa[da == 'down'], 'o', color=col, ms=4, label='%s down: max |res| %.2f %%' % (c, d['max_abs_residual_pct_5_70']))
            if (da == 'up').any(): ax[0, j].semilogx(fsa[da == 'up'], rsa[da == 'up'], '^', color=col, ms=5, mfc='none', label='%s up%s' % (c, '' if not d.get('up_down') else ': mean up-down %+.2f %%' % d['up_down']['mean_up_minus_down_pct']))
            ax[1, j].loglog(np.array(cv['S']).clip(1), np.array(cv['var']).clip(1e-3), 'o', color=col, ms=4, label='%s  gain %.4f e/ADU, read noise %.2f e' % (c, d['gain_e_per_adu'], d['read_noise_e']))
        ax[0, j].axhline(0, color='k', lw=.5); ax[0, j].set_ylim(-5, 5); ax[0, j].set_xlabel('signal / full scale'); ax[0, j].set_ylabel('deviation from linear (%)'); ax[0, j].set_title('ISO %s: linearity' % iso); ax[0, j].legend(fontsize=8); ax[0, j].grid(alpha=.3, which='both')
        ax[1, j].set_xlabel('signal (ADU above black)'); ax[1, j].set_ylabel('variance of one frame (ADU^2)'); ax[1, j].set_title('ISO %s: photon transfer' % iso); ax[1, j].legend(fontsize=8); ax[1, j].grid(alpha=.3, which='both')
    plt.tight_layout(); plt.savefig(path, dpi=110)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out'); ap.add_argument('--isos', default='800'); ap.add_argument('--cycles', type=int, default=4); ap.add_argument('--every', type=int, default=120)
    ap.add_argument('--port', type=int, default=int(os.environ.get('ORIGIN_CONTROL_PORT', 8765))); ap.add_argument('--keep', action='store_true'); ap.add_argument('--analyse'); ap.add_argument('--down-only', action='store_true', help='skip the pass back up')
    a = ap.parse_args(); log = lambda s: print(time.strftime('%H:%M:%S'), s, flush=True)
    if a.analyse:
        recs = json.load(open(a.analyse)); out = os.path.dirname(os.path.abspath(a.analyse))
    else:
        if not a.out: ap.error('--out is required')
        os.makedirs(a.out, exist_ok=True); out = a.out
        recs = run(Scope(a.port, a.out, a.keep), [int(i) for i in a.isos.split(',')], a.cycles, a.every, log, not a.down_only); json.dump(recs, open(os.path.join(out, 'records.json'), 'w'))
    res = analyse(recs); json.dump(res, open(os.path.join(out, 'linearity.json'), 'w'), indent=1); plot(res, os.path.join(out, 'linearity.png'))
    for iso, d in res.items():
        for c, v in d.items(): log('ISO %s %s: %s' % (iso, c, v['error'] if 'error' in v else 'black %.0f ADU, linearity max %.2f %% rms %.2f %% (5-70 %% of scale), gain %.4f e/ADU, read noise %.1f ADU = %.2f e%s' % (v['black_level_adu'], v['max_abs_residual_pct_5_70'], v['rms_residual_pct_5_70'], v['gain_e_per_adu'], v['read_noise_adu'], v['read_noise_e'], '' if not v.get('up_down') else '; up minus down: mean %+.2f %%, max |%.2f| %% over %d matched points, by band %s' % (v['up_down']['mean_up_minus_down_pct'], v['up_down']['max_abs_pct'], v['up_down']['matched'], v['up_down']['by_band_pct']))))


if __name__ == '__main__':
    main()
