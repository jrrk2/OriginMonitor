#!/usr/bin/env python3
"""Move raw FITS frames to a backup tree, losslessly compressed, deleting each source only after it has been verified.

    nice -n 19 ionice -c3 tools/fits_move_to_backup.py SRC_DIR DST_DIR --jobs 2 --log move.jsonl

Per file: fpack (GZIP_2 tiles, lossless) to DST/<name>.fits.fz.tmp; reopen it and require every pixel and every header keyword (except the
added checksums) to equal the source; rename to .fits.fz, restore the source timestamps, fsync; only then remove the source. A source is never
removed if anything differs, fails or looks unusual. Resumable: files already present as .fits.fz are verified against the source before the
source is removed. Stops if DST has less than --min-free-gb free.
"""
import argparse, json, os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fits_compress_backup import verify

def move(src, dst_dir, min_free):
    out = {'path': src, 'orig': os.path.getsize(src)}; t0 = time.time()
    final = os.path.join(dst_dir, os.path.basename(src)[:-5] + '.fits.fz'); tmp = final + '.tmp'
    try:
        if shutil.disk_usage(dst_dir).free < min_free * 1e9: out['status'] = 'STOP: low disk space'; return out
        st = os.stat(src)
        if time.time() - st.st_mtime < 120: out['status'] = 'skipped: modified in the last 2 minutes'; return out
        if not os.path.exists(final):
            with open(tmp, 'wb') as f:
                r = subprocess.run(['fpack', '-g2', '-q', '0', '-S', src], stdout=f, stderr=subprocess.PIPE)
            if r.returncode != 0: out['status'] = 'fpack failed: ' + r.stderr.decode()[:100]; os.path.exists(tmp) and os.remove(tmp); return out
            bad = verify(src, tmp)
            if bad: out['status'] = 'VERIFY FAILED: ' + bad; os.remove(tmp); return out
            os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns)); os.chmod(tmp, 0o644); os.rename(tmp, final)
            with open(final, 'rb') as f: os.fsync(f.fileno())
        else:
            bad = verify(src, final)                                        # resuming: an existing copy must verify before the source goes
            if bad: out['status'] = 'EXISTING COPY DIFFERS: ' + bad; return out
        out['comp'] = os.path.getsize(final); out['ratio'] = round(out['orig'] / out['comp'], 3)
        os.remove(src); out['status'] = 'moved'
    except Exception as e:
        out['status'] = 'error: %s' % e
        try: os.path.exists(tmp) and os.remove(tmp)
        except OSError: pass
    out['seconds'] = round(time.time() - t0, 2); return out

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('--jobs', type=int, default=2); ap.add_argument('--log', default='move.jsonl'); ap.add_argument('--min-free-gb', type=float, default=5.0); ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args(); os.makedirs(a.dst, exist_ok=True)
    files = sorted(os.path.join(a.src, f) for f in os.listdir(a.src) if f.endswith('.fits')); files = files[:a.limit] if a.limit else files
    print(len(files), 'FITS files to move from', a.src, 'to', a.dst, flush=True); logf = open(a.log, 'a'); n = 0; to = tc = 0; t0 = time.time(); stats = {}
    with ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(lambda p: move(p, a.dst, a.min_free_gb), files):
            logf.write(json.dumps(r) + '\n'); logf.flush(); n += 1; k = r['status'].split(':')[0]; stats[k] = stats.get(k, 0) + 1
            if r['status'] == 'moved': to += r['orig']; tc += r['comp']
            if r['status'].startswith('STOP'): print('stopping:', r['status']); break
            if n % 250 == 0: print('%d files, %.1f GB -> %.1f GB (%.2fx), %.0f s, %s' % (n, to / 1e9, tc / 1e9, to / max(tc, 1), time.time() - t0, stats), flush=True)
    print('\nsummary:', stats); print('moved %.1f GB -> %.1f GB on the backup (%.2fx)' % (to / 1e9, tc / 1e9, to / max(tc, 1)))
    for l in open(a.log):
        r = json.loads(l)
        if r['status'] not in ('moved',) and not r['status'].startswith('skipped'): print('  NOT MOVED:', r['path'].split('/')[-1], '-', r['status'][:90])

if __name__ == '__main__':
    main()
