#!/usr/bin/env python3
"""Turn 3-plane debayered FITS (CCDciel/INDI Origin captures, 16-bit) back into the single Bayer mosaic they were made from.

    tools/rgb_to_bayer.py ROOT [--originals DIR] [--dry-run]

The debayer that wrote them keeps each colour's own sites untouched, fills R and B bilinearly, fills G edge-directed
(the horizontal or vertical pair, whichever differs less), and copies the outermost row/column. So the mosaic holds
everything: for every file the mosaic is re-debayered the same way and compared with all 3 planes; a file is replaced
only if the reconstruction matches to within 2 ADU (rounding) at 99.9% of pixels, away from the 1 px border which the
debayer had already overwritten. The phase (which site holds which colour) is found per file and written as BAYERPAT.
fpacked (.fz) files are written fpacked again (lossless Rice). Originals are moved (a rename) to --originals (default ROOT/_rgb_originals), same relative paths, to delete once you are happy.
"""
import argparse, os, sys
import numpy as np
from astropy.io import fits

SITES = [(0, 0), (0, 1), (1, 0), (1, 1)]


def debayer(M, rsite, bsite):
    """the same interpolation, interior only (border left as NaN)"""
    H, W = M.shape; out = np.full((3, H, W), np.nan)
    yy, xx = np.mgrid[0:H, 0:W]
    for c, (sy, sx) in ((0, rsite), (2, bsite)):
        P = np.full((H, W), np.nan); m = (yy % 2 == sy) & (xx % 2 == sx); P[m] = M[m]
        h = (np.roll(P, 1, 1) + np.roll(P, -1, 1)) / 2; v = (np.roll(P, 1, 0) + np.roll(P, -1, 0)) / 2
        d = (np.roll(np.roll(P, 1, 0), 1, 1) + np.roll(np.roll(P, 1, 0), -1, 1) + np.roll(np.roll(P, -1, 0), 1, 1) + np.roll(np.roll(P, -1, 0), -1, 1)) / 4
        o = np.where(m, P, np.where(yy % 2 == sy, h, np.where(xx % 2 == sx, v, d)))
        out[c] = o
    gm = ~(((yy % 2 == rsite[0]) & (xx % 2 == rsite[1])) | ((yy % 2 == bsite[0]) & (xx % 2 == bsite[1])))
    l, r, u, dn = np.roll(M, 1, 1), np.roll(M, -1, 1), np.roll(M, 1, 0), np.roll(M, -1, 0)
    g = np.where(np.abs(l - r) <= np.abs(u - dn), (l + r) / 2, (u + dn) / 2)          # a tie goes horizontal
    out[1] = np.where(gm, M, g)
    out[:, :2] = out[:, -2:] = out[:, :, :2] = out[:, :, -2:] = np.nan
    return out


def phase(d):
    """the site of each plane that is NOT an average of its neighbours"""
    res = []
    for p in d:
        sc = {}
        for (i, j) in SITES:
            s = p[2 + i:-2:2, 2 + j:-2:2]
            h = p[2 + i:-2:2, 1 + j:-3:2] + p[2 + i:-2:2, 3 + j:-1:2]; v = p[1 + i:-3:2, 2 + j:-2:2] + p[3 + i:-1:2, 2 + j:-2:2]
            sc[(i, j)] = np.mean((np.abs(2 * s - h) <= 2) | (np.abs(2 * s - v) <= 2))
        res.append(sorted(sc, key=sc.get))
    return res


def convert(path, args):
    with fits.open(path) as hl:
        packed = len(hl) == 2 and isinstance(hl[1], fits.CompImageHDU) and hl[0].data is None      # fpack (.fz): the image is the compressed extension
        hdu = hl[1] if packed else hl[0]
        hdr = hdu.header.copy(); d = hdu.data
        if hdr.get('NAXIS') != 3 or hdr.get('NAXIS3') != 3 or hdr.get('BITPIX') != 16 or (len(hl) != 1 and not packed):
            return 'skip (not a 16-bit 3-plane image)'
        d = d.astype(np.int64)
    ph = phase(d); rsite, gsites, bsite = ph[0][0], sorted(ph[1][:2]), ph[2][0]
    if sorted([rsite, bsite] + gsites) != SITES:      # e.g. a plane clipped flat at 65535: take the usual phase, still checked below
        rsite, gsites, bsite = (1, 1), [(0, 1), (1, 0)], (0, 0)
    M = np.empty(d.shape[1:], np.int64)
    for c, sites in ((0, [rsite]), (1, gsites), (2, [bsite])):
        for (i, j) in sites: M[i::2, j::2] = d[c, i::2, j::2]
    rec = debayer(M.astype(np.float64), rsite, bsite); ok = ~np.isnan(rec)
    err = np.abs(rec[ok] - d[ok]); p999 = np.percentile(err, 99.9); worst = err.max(); frac = np.mean(err > 2)
    if p999 > 2:
        return f'skip (reconstruction 99.9% error {p999:.1f} ADU, max {worst:.0f})'
    pat = [''] * 4
    for c, sites in (('R', [rsite]), ('G', gsites), ('B', [bsite])):
        for (i, j) in sites: pat[2 * i + j] = c
    pat = ''.join(pat)
    msg = f'{pat}, re-debayer max error {worst:.1f} ADU ({frac:.3%} > 2)'
    if args.dry_run: return 'ok ' + msg
    for k in ('NAXIS3',): hdr.remove(k, ignore_missing=True)
    hdr['NAXIS'] = 2
    hdr['BAYERPAT'] = (pat, 'Bayer colour pattern, first row/col in file')
    hdr['XBAYROFF'] = (0, 'X offset of Bayer array'); hdr['YBAYROFF'] = (0, 'Y offset of Bayer array')
    hdr['HISTORY'] = 'Reduced from 3 debayered planes to the Bayer mosaic (tools/rgb_to_bayer.py)'
    tmp = path + '.bayer.tmp'
    fits.PrimaryHDU(M.astype(np.uint16), header=hdr).writeto(tmp, overwrite=True, output_verify='silentfix')
    if packed:                                    # fpack it again with fpack itself (lossless Rice, its default), so funpack gives back a primary image as before
        import subprocess
        with open(tmp + '.fz', 'wb') as o: rc = subprocess.run(['fpack', '-S', tmp], stdout=o, stderr=subprocess.PIPE).returncode
        os.remove(tmp)
        if rc: os.remove(tmp + '.fz'); return 'skip (fpack failed)'
        os.rename(tmp + '.fz', tmp)
    with fits.open(tmp) as hl: chk = (hl[1] if packed else hl[0]).data.astype(np.int64)
    if not np.array_equal(chk, M): os.remove(tmp); return 'skip (write check failed)'
    if args.delete_originals: os.remove(path)
    else:
        dst = os.path.join(args.originals, os.path.relpath(path, args.root)); os.makedirs(os.path.dirname(dst), exist_ok=True); os.rename(path, dst)
    os.rename(tmp, path)
    return 'ok ' + msg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root'); ap.add_argument('--originals'); ap.add_argument('--dry-run', action='store_true'); ap.add_argument('--limit', type=int); ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--delete-originals', action='store_true', help='delete each original once its mosaic is verified, instead of moving it to --originals (a full disk)')
    a = ap.parse_args(); a.root = os.path.abspath(a.root)
    a.originals = os.path.abspath(a.originals or os.path.join(a.root, '_rgb_originals'))
    files = sorted(os.path.join(r, f) for r, _, fs in os.walk(a.root) if not os.path.abspath(r).startswith(a.originals)
                   for f in fs if f.lower().endswith(('.fit', '.fits', '.fts', '.fit.fz', '.fits.fz', '.fts.fz')))[:a.limit]
    n = {}
    from concurrent.futures import ProcessPoolExecutor
    with ProcessPoolExecutor(a.jobs) as ex:
        for f, r in zip(files, ex.map(one, [(f, a) for f in files])):
            n[r.split(' ')[0]] = n.get(r.split(' ')[0], 0) + 1
            print(os.path.relpath(f, a.root), r, flush=True)
    print(n)


def one(fa):
    try: return convert(*fa)
    except Exception as e: return f'skip (error {e})'


if __name__ == '__main__':
    main()
