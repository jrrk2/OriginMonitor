#!/usr/bin/env python3
"""Lossless in-place FITS compression for a backup tree (fpack, GZIP_2 tiles), with per-file verification.

    tools/fits_compress_backup.py ROOT --pilot 60            # random sample, writes to a scratch dir, touches nothing in ROOT
    nice -n 19 ionice -c3 tools/fits_compress_backup.py ROOT --apply --jobs 3 --log compress.jsonl

For each *.fits (single image HDU only): compress to <name>.fz.tmp, reopen it and require every pixel and every header
keyword (except checksums) to equal the original; only then rename to <name>.fits.fz, restore the original timestamps and
remove the original. Files that don't compress usefully (< --min-ratio), floating-point files with a poor ratio, files that fail
verification, or anything unexpected are left untouched and logged. Resumable: a .fits.fz next to a .fits is never overwritten.
"""
import argparse, json, os, random, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from astropy.io import fits

SKIP_KEYS = {'CHECKSUM', 'DATASUM'}
EXCLUDE = ('/venv/', '/.venv/', 'site-packages', '/.git/', '/node_modules/', '/__pycache__/', '/lost+found/', '/tests/data')   # never touch tooling or test data

def verify(orig, comp):
    with fits.open(orig, memmap=False, do_not_scale_image_data=True) as a, fits.open(comp, memmap=False, do_not_scale_image_data=True) as b:
        if len(a) != 1: return 'multi-HDU original'
        ha = a[0].header; hb = b[1].header if len(b) > 1 else b[0].header
        da = a[0].data; db = b[1].data if len(b) > 1 else b[0].data
        # FITS arrays are big-endian on disk; a decompressed copy may come back native-endian. Same values, so compare kind and size, not byte order.
        if da is None or db is None or da.shape != db.shape or da.dtype.kind != db.dtype.kind or da.dtype.itemsize != db.dtype.itemsize: return 'shape/dtype mismatch %s %s' % (getattr(da, 'dtype', None), getattr(db, 'dtype', None))
        if not np.array_equal(da.astype(da.dtype.newbyteorder('=')), db.astype(db.dtype.newbyteorder('='))): return 'pixel data differ'
        if da.dtype.kind == 'f' and not np.array_equal(np.isnan(da), np.isnan(db)): return 'NaN pattern differs'
        ka = {k: ha[k] for k in ha if k and k not in SKIP_KEYS and k not in ('COMMENT', 'HISTORY', '')}
        kb = {k: hb[k] for k in hb if k and k not in SKIP_KEYS and k not in ('COMMENT', 'HISTORY', '')}
        # compressed images carry structural keywords (EXTNAME/XTENSION...) that the original lacks; compare the original's keywords only
        for k, v in ka.items():
            if k not in kb or kb[k] != v: return 'header keyword %s differs (%r vs %r)' % (k, v, kb.get(k))
    return None

def one(path, scratch=None, apply=False, min_ratio=1.15, float_min_ratio=1.3):
    t0 = time.time(); st = os.stat(path); out = {'path': path, 'orig': st.st_size}
    if os.path.islink(path) or st.st_nlink > 1: out['status'] = 'skipped: symlink or hard-linked'; return out
    final = path[:-5] + '.fits.fz' if path.endswith('.fits') else path + '.fz'
    if os.path.exists(final): out['status'] = 'skipped: .fz already exists'; return out
    tmp = (os.path.join(scratch, os.path.basename(path) + '.fz') if scratch else path + '.fz.tmp')
    try:
        with fits.open(path, memmap=False, do_not_scale_image_data=True) as h:
            if len(h) != 1 or h[0].data is None: out['status'] = 'skipped: not a single image HDU'; return out
            out['bitpix'] = int(h[0].header['BITPIX'])
        with open(tmp, 'wb') as f:
            r = subprocess.run(['fpack', '-g2', '-q', '0', '-S', path], stdout=f, stderr=subprocess.PIPE)
        if r.returncode != 0: out['status'] = 'fpack failed: ' + r.stderr.decode()[:120]; os.path.exists(tmp) and os.remove(tmp); return out
        out['comp'] = os.path.getsize(tmp); out['ratio'] = round(out['orig'] / out['comp'], 3)
        need = float_min_ratio if out['bitpix'] < 0 else min_ratio
        if out['ratio'] < need: out['status'] = 'kept: ratio %.2f below %.2f' % (out['ratio'], need); os.remove(tmp); return out
        bad = verify(path, tmp)
        if bad: out['status'] = 'VERIFY FAILED: ' + bad; os.remove(tmp); return out
        if apply and not scratch:
            os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns)); os.chmod(tmp, st.st_mode & 0o7777)
            os.rename(tmp, final)
            with open(final, 'rb') as f: os.fsync(f.fileno())
            os.remove(path); out['status'] = 'compressed'
        else:
            out['status'] = 'verified (pilot)'; os.remove(tmp)
    except Exception as e:
        out['status'] = 'error: %s' % e
        try: os.path.exists(tmp) and os.remove(tmp)
        except OSError: pass
    out['seconds'] = round(time.time() - t0, 2); return out

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root'); ap.add_argument('--pilot', type=int, default=0); ap.add_argument('--apply', action='store_true'); ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--min-ratio', type=float, default=1.15); ap.add_argument('--log', default='compress.jsonl'); ap.add_argument('--scratch', default='')
    ap.add_argument('--subdir', default='', help='only this subdirectory of ROOT'); ap.add_argument('--limit', type=int, default=0); ap.add_argument('--min-free-gb', type=float, default=3.0)
    a = ap.parse_args()
    files = []
    for dp, dn, fn in os.walk(os.path.join(a.root, a.subdir)):
        for f in fn:
            fp = os.path.join(dp, f)
            if f.lower().endswith('.fits') and not f.startswith('.') and not any(x in fp + '/' for x in EXCLUDE): files.append(fp)
    files.sort(); print(len(files), 'FITS files under', os.path.join(a.root, a.subdir), flush=True)
    if a.pilot:
        random.seed(1); files = random.sample(files, min(a.pilot, len(files))); os.makedirs(a.scratch, exist_ok=True)
    elif a.limit: files = files[:a.limit]
    done = set()
    if os.path.exists(a.log):
        for l in open(a.log):
            try: d = json.loads(l); done.add(d['path'])
            except Exception: pass
    files = [f for f in files if f not in done or a.pilot]
    logf = open(a.log, 'a'); tot_o = tot_c = n = 0; t0 = time.time(); stats = []
    def work(p):
        if shutil.disk_usage(os.path.dirname(p)).free < a.min_free_gb * 1e9: return {'path': p, 'status': 'STOP: low disk space'}
        return one(p, a.scratch or None, a.apply and not a.pilot, a.min_ratio)
    with ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(work, files):
            logf.write(json.dumps(r) + '\n'); logf.flush(); n += 1; stats.append(r)
            if r.get('status', '').startswith('STOP'): print('stopping:', r['status']); break
            if 'comp' in r and r['status'] in ('compressed', 'verified (pilot)'): tot_o += r['orig']; tot_c += r['comp']
            if n % 200 == 0 or a.pilot and n % 10 == 0: print('%d files, %.1f -> %.1f GB (%.2fx), %.0f s' % (n, tot_o / 1e9, tot_c / 1e9, tot_o / max(tot_c, 1), time.time() - t0), flush=True)
    st = {}
    for r in stats: st[r.get('status', '?').split(':')[0]] = st.get(r.get('status', '?').split(':')[0], 0) + 1
    print('\nsummary:', st); 
    ok = [r for r in stats if 'comp' in r]; 
    if ok: print('overall ratio on processed: %.2fx; mean %.1f s per file' % (sum(r['orig'] for r in ok) / sum(r['comp'] for r in ok), np.mean([r.get('seconds', 0) for r in ok])))
    for bp in sorted({r.get('bitpix') for r in ok if 'bitpix' in r}): 
        g = [r for r in ok if r.get('bitpix') == bp]; print('  BITPIX %3d: %d files, ratio %.2fx' % (bp, len(g), sum(r['orig'] for r in g) / sum(r['comp'] for r in g)))
    bad = [r for r in stats if 'FAIL' in r.get('status', '') or r.get('status', '').startswith('error')]
    for r in bad[:10]: print('  PROBLEM:', r['path'], r['status'])

if __name__ == '__main__':
    main()
