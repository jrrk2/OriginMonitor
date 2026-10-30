#!/usr/bin/env python3
"""Stamp the reference filter into live stacks saved before the stack state recorded it (state.bin LST1/LST2 -> LST3, see LiveStacker::save).

    tools/stack_filter.py [--data /mnt/fits-backup/claude] [--apply] [--set DIR=FILTER ...]

The daemon now archives a stack whose reference filter differs from the incoming frame's and starts a new one (SkyMonitor::process). A stack with no recorded filter is
never archived, so a pre-filter stack keeps reading every quad-band frame as cloud. For each stacks/<dir>/state.bin without a filter this finds the frame that seeded it:
the last focus_log.csv row with stack_frames = 1 for that target (the directory name without an archive suffix _YYYYMMDD_HHMMSS or _prefilter_..., as SkyMonitor names it)
before state.bin was last written, matched to its snapshot by time, and reads that frame's FITS FILTER. --set overrides a directory. Without --apply it only reports.
Stop the daemon first, or make sure it holds none of these stacks: it saves the stack it holds when the target changes.
"""
import argparse, csv, glob, os, re, struct, sys
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from restack import snapshot_index                                   # snapshot files by the UTC time of their name

LST1, LST2, LST3 = 0x4C535431, 0x4C535432, 0x4C535433


def safe(s):
    return ''.join(c if c.isalnum() or c in '-_' else '_' for c in s) or 'target'


def read_magic(path):
    with open(path, 'rb') as f: return struct.unpack('<I', f.read(4))[0]


def rewrite(path, flt):
    b = open(path, 'rb').read()
    magic, w, h, n = struct.unpack_from('<Iiii', b, 0); o = 16; iso = 800
    if magic == LST2: (iso,) = struct.unpack_from('<i', b, o); o += 4
    f = flt.encode()[:64]
    out = struct.pack('<Iiiiii', LST3, w, h, n, iso, len(f)) + f + b[o:]
    tmp = path + '.tmp'; open(tmp, 'wb').write(out); os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', default=os.environ.get('ORIGIN_DATA_DIR', '/mnt/fits-backup/claude')); ap.add_argument('--apply', action='store_true')
    ap.add_argument('--set', action='append', default=[])
    a = ap.parse_args()
    manual = dict(s.split('=', 1) for s in a.set)
    from astropy.io import fits
    seeds = {}
    with open(os.path.join(a.data, 'focus_log.csv')) as fh:
        for r in csv.DictReader(fh):
            if r['stack_frames'] == '1': seeds.setdefault(safe(r['target']), []).append(datetime.fromisoformat(r['time'].replace('Z', '+00:00')))
    snaps = snapshot_index(a.data)
    for st in sorted(glob.glob(os.path.join(a.data, 'stacks', '*', 'state.bin'))):
        d = os.path.basename(os.path.dirname(st)); m = read_magic(st)
        if m == LST3: print(f'{d:45s} already LST3'); continue
        flt, why = manual.get(d), 'set by hand'
        if not flt:
            base = re.sub(r'(_prefilter)?(_\d{8})?(_\d{6})?$', '', d)
            saved = datetime.fromtimestamp(os.path.getmtime(st), timezone.utc)
            ts = [t for t in seeds.get(base, []) if t <= saved + timedelta(seconds=5)]
            if not ts: print(f'{d:45s} no seeding frame found for "{base}": left unknown (use --set {d}=Clear|Nebula)'); continue
            t = max(ts); snap = min(snaps, key=lambda s: abs((s[0] - t).total_seconds()))
            if abs((snap[0] - t).total_seconds()) > 4: print(f'{d:45s} seeding frame {t:%Y-%m-%d %H:%M:%S} has no snapshot file: left unknown'); continue
            flt = str(fits.getheader(snap[1]).get('FILTER', '') or ''); why = f'seeded {t:%Y-%m-%d %H:%M} by {os.path.basename(snap[1])}'
            if not flt: print(f'{d:45s} {why}: no FILTER in its header, left unknown'); continue
        print(f'{d:45s} {flt:7s} {why}' + ('' if a.apply else '  (dry run)'))
        if a.apply: rewrite(st, flt)


if __name__ == '__main__':
    main()
