#!/usr/bin/env python3
"""Move the flat DATA/snapshot_* frames into DATA/frames/<target>/, as OriginHeadless writes them since 2026-11-23, and rewrite the paths recorded in the logs.

    tools/frames_by_target.py [--data /mnt/fits-backup/claude]            what it would do (nothing is changed)
    tools/frames_by_target.py --apply                                     move the frames and rewrite the logs
    tools/frames_by_target.py --undo DATA/frames/moves_<UTC>.tsv          put them back and restore the old paths

Target: the focus_log.csv row analysed just after the frame was downloaded (the same match as tools/restack.py frames(), but for every verdict), so the
folder is the target the live stacker had then. A mosaic tile ("<mosaic> i,j", names from DATA/mosaic_*.jsonl) goes to its mosaic's folder, as SkyMonitor does. Frames with no row go to frames/unsorted. The folder name is SkyMonitor's safeName() of the target.
Paths are rewritten in every text file under DATA (logs, manifests, frame lists) that names a moved file; the moves are listed in frames/moves_<UTC>.tsv for --undo.
Run it in the day: a live run writing frames at the same time is not handled.
"""
import argparse, bisect, csv, json, os, re, sys
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from restack import LONDON

SKIP_DIRS = {'allsky', 'allsky_night', 'allsky_labelled', 'frames'}
SKIP_EXT = ('.fits', '.fit', '.fts', '.png', '.jpg', '.jpeg', '.tif', '.tiff', '.bin', '.npy', '.npz', '.gz', '.mp4', '.gif', '.pdf', '.docx', '.bsp')


def safe_name(s):
    o = ''.join(c if c.isalnum() or c in '-_' else '_' for c in s)
    return o or 'target'


def frame_time(name):
    try: return datetime.strptime(name[9:28], '%Y%m%d_%H%M%S_%f').replace(tzinfo=LONDON).astimezone(timezone.utc)
    except ValueError: return None


def mosaic_names(data):
    names = set()
    for f in os.listdir(data):
        if f.startswith('mosaic_') and f.endswith('.jsonl'):
            with open(os.path.join(data, f)) as fh:
                for l in fh:
                    if 'mosaic_start' not in l: continue
                    try: e = json.loads(l)
                    except ValueError: continue
                    if e.get('name'): names.add(e['name'])
    return names


def folder(target, mosaics):
    """a tile's target "<mosaic> i,j" appears in focus_log.csv cut at its comma ("<mosaic> i", 3-14 Nov): either way the mosaic's name"""
    for m in mosaics:
        if re.fullmatch(re.escape(m) + r' \d+(,\d+)?', target): return safe_name(m)
    return safe_name(target)


def plan(data):
    mosaics = mosaic_names(data)
    snaps = sorted((t, f) for f in os.listdir(data) if f.startswith('snapshot_') and os.path.isfile(os.path.join(data, f)) for t in [frame_time(f)] if t)
    times = [t for t, _ in snaps]; target = {}
    with open(os.path.join(data, 'focus_log.csv'), newline='') as fh:
        for r in csv.DictReader(fh):
            try: t = datetime.fromisoformat(r['time'].replace('Z', '+00:00'))
            except (ValueError, KeyError, AttributeError): continue
            i = bisect.bisect_left(times, t - timedelta(seconds=4)); best = None
            while i < len(snaps) and snaps[i][0] <= t + timedelta(seconds=0.5):   # downloaded (named) just before it was analysed
                if snaps[i][1] not in target and (best is None or abs((snaps[i][0] - t).total_seconds()) < abs((best[0] - t).total_seconds())): best = snaps[i]
                i += 1
            if best: target[best[1]] = r.get('target') or 'auto'
    # a mosaic tile name with a comma was logged unquoted (3-4 Nov) and its target column is then the tile's row: still a name, kept as it is
    return [(f, 'unsorted' if f not in target else folder(target[f], mosaics)) for _, f in snaps]


def text_files(data):
    for root, dirs, files in os.walk(data):
        if root == data: dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if f.lower().endswith(SKIP_EXT) or f.startswith('snapshot_'): continue
            yield os.path.join(root, f)


def rewrite(data, mapping, dry):
    """replace DATA/<old name> by its new path in every text file that names one; mapping: old absolute path -> new absolute path"""
    pat = re.compile(re.escape(data.rstrip('/') + '/') + r'(snapshot_[0-9_]+\.[A-Za-z]+)')
    names = {os.path.basename(o): n for o, n in mapping.items()}
    changed = refs = 0
    for p in text_files(data):
        try:
            with open(p, 'rb') as fh: raw = fh.read()
            if b'snapshot_' not in raw: continue
            s = raw.decode('utf-8')
        except (UnicodeDecodeError, OSError): continue
        n = [0]
        def sub(m):
            new = names.get(m.group(1))
            if new is None: return m.group(0)
            n[0] += 1; return new
        out = pat.sub(sub, s)
        if not n[0]: continue
        changed += 1; refs += n[0]
        if not dry:
            st = os.stat(p); tmp = p + '.frames_tmp'
            with open(tmp, 'w', encoding='utf-8', newline='') as fh: fh.write(out)
            os.chmod(tmp, st.st_mode); os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns)); os.replace(tmp, p)
    return changed, refs


def undo_rewrite(data, back):
    """new absolute path -> old absolute path, in every text file"""
    pat = re.compile(re.escape(data.rstrip('/') + '/frames/') + r'[^/"\'\s,]+/(snapshot_[0-9_]+\.[A-Za-z]+)')
    changed = 0
    for p in text_files(data):
        try:
            with open(p, 'rb') as fh: raw = fh.read()
            if b'/frames/' not in raw: continue
            s = raw.decode('utf-8')
        except (UnicodeDecodeError, OSError): continue
        out = pat.sub(lambda m: back.get(m.group(0), m.group(0)), s)
        if out != s:
            changed += 1; st = os.stat(p); tmp = p + '.frames_tmp'
            with open(tmp, 'w', encoding='utf-8', newline='') as fh: fh.write(out)
            os.chmod(tmp, st.st_mode); os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns)); os.replace(tmp, p)
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', default=os.environ.get('ORIGIN_DATA_DIR', '/mnt/fits-backup/claude'))
    ap.add_argument('--apply', action='store_true'); ap.add_argument('--undo', metavar='MOVES_TSV')
    a = ap.parse_args(); data = os.path.abspath(a.data)

    if a.undo:
        rows = [l.rstrip('\n').split('\t') for l in open(a.undo) if l.strip() and not l.startswith('#')]
        back = {new: old for old, new in rows}
        print(f'paths restored in {undo_rewrite(data, back)} files')
        moved = 0
        for old, new in rows:
            if os.path.exists(new) and not os.path.exists(old): os.rename(new, old); moved += 1
        print(f'{moved} of {len(rows)} frames moved back')
        return

    moves = plan(data)
    if not moves: print('no flat snapshot_* files in', data); return
    by = {}
    for _, t in moves: by[t] = by.get(t, 0) + 1
    print(f'{len(moves)} frames into {len(by)} folders under {data}/frames/:')
    for t, n in sorted(by.items(), key=lambda x: -x[1]): print(f'  {n:6d}  {t}')
    mapping = {os.path.join(data, f): os.path.join(data, 'frames', t, f) for f, t in moves}
    clash = [n for n in mapping.values() if os.path.exists(n)]
    if clash: sys.exit(f'{len(clash)} destinations already exist, e.g. {clash[0]}: nothing changed')
    files, refs = rewrite(data, mapping, dry=True)
    print(f'paths to rewrite: {refs} in {files} files')
    if not a.apply: print('dry run: nothing changed (--apply to do it)'); return

    os.makedirs(os.path.join(data, 'frames'), exist_ok=True)
    log = os.path.join(data, 'frames', 'moves_%s.tsv' % datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S'))
    with open(log, 'w') as fh:
        fh.write('# old\tnew   (tools/frames_by_target.py --undo this file)\n')
        for o, n in mapping.items(): fh.write(f'{o}\t{n}\n')
    for o, n in mapping.items():          # the moves first: a file that is still where the logs say is never lost, and --undo puts back any partial state
        os.makedirs(os.path.dirname(n), exist_ok=True); os.rename(o, n)
    files, refs = rewrite(data, mapping, dry=False)
    print(f'moved {len(mapping)} frames; rewrote {refs} paths in {files} files; undo list {log}')


if __name__ == '__main__':
    main()
