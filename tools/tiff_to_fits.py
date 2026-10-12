#!/usr/bin/env python3
"""Turn the pre-debayered 16-bit RGB TIFF snapshots of the older Origin interface back into raw Bayer-mosaic FITS.

    tools/tiff_to_fits.py SRC OUT [--compress] [--limit N]        SRC: a TIFF file or a folder of *.tif(f); OUT: output folder

The old interface debayered by interpolation, and every native sample survives unchanged in the TIFF (bilinear: the other pixels are exact averages of neighbours), so nothing is lost by
keeping only the sample each pixel really measured. The lattice is detected from the data (the pixels that are NOT an exact neighbour average are the native ones); for the Origin 1.2.x TIFFs
it is B G / G R (B at row 0, column 0), which the analyser detects by itself. The TIFFs had the black level removed, so BLKLEVEL = 4985 is added back to give the raw 16-bit scale the
analyser expects. Header: EXPTIME, ISOSPEED, DATE-OBS (UTC, from the file name's epoch milliseconds when it has one, else the EXIF time), CREATOR, LATITUDE/LONGITUD, BAYERPAT. The output is
uint16 with the usual BZERO 32768 convention; --compress runs fpack (lossless Rice) and leaves only <name>.fits.fz.
"""
import argparse, glob, os, re, subprocess, sys
from datetime import datetime, timezone
import numpy as np
import cv2
import tifffile
from astropy.io import fits

BLACK = 4985

def detect_pattern(rgb):
    """Return (pattern, confidence) from an RGB uint16 array: native sites are those where the channel is not an exact average of its neighbours."""
    c = rgb[:, :, 1].astype(np.int64); H, W = c.shape; a = c[2:H - 2, 2:W - 2]
    hav = (c[2:H - 2, 1:W - 3] + c[2:H - 2, 3:W - 1]) // 2; vav = (c[1:H - 3, 2:W - 2] + c[3:H - 1, 2:W - 2]) // 2        # green is interpolated at R and B sites, edge-directed: horizontal or vertical average
    m = (np.abs(a - hav) <= 1) | (np.abs(a - vav) <= 1)
    fr = np.array([[m[i::2, j::2].mean() for j in range(2)] for i in range(2)])                                    # high where green is interpolated (a red or blue site), low where native
    inter = [(i, j) for i in range(2) for j in range(2) if fr[i, j] >= fr.min() + 0.2]
    if len(inter) != 2 or (inter[0][0] + inter[0][1]) % 2 != (inter[1][0] + inter[1][1]) % 2: return None, 0.0
    # the two interpolated-green sites are the red and blue ones; which is which: red is native in the red channel only
    red = rgb[:, :, 0].astype(np.int64); H2, W2 = red.shape
    ra = red[2:H2 - 2, 2:W2 - 2]; rx = (red[1:H2 - 3, 1:W2 - 3] + red[3:H2 - 1, 3:W2 - 1] + red[1:H2 - 3, 3:W2 - 1] + red[3:H2 - 1, 1:W2 - 3]) // 4
    mm = np.abs(ra - rx) <= 1; rf = np.array([[mm[i::2, j::2].mean() for j in range(2)] for i in range(2)])
    rsite = min(inter, key=lambda s: rf[s])                                                                         # the red channel is native (not an average) at the red site
    bsite = [s for s in inter if s != rsite][0]
    grid = [['G', 'G'], ['G', 'G']]; grid[rsite[0]][rsite[1]] = 'R'; grid[bsite[0]][bsite[1]] = 'B'
    return ''.join(grid[0] + grid[1]), float(fr.max() - fr.min())

def convert(src, out, pattern=None, compress=False):
    im = cv2.imread(src, cv2.IMREAD_UNCHANGED)
    if im is None or im.ndim != 3: raise ValueError('not an RGB TIFF')
    rgb = im[:, :, ::-1]                                                                                           # OpenCV gives BGR
    pat, conf = detect_pattern(rgb) if pattern is None else (pattern, 1.0)
    if pat is None: pat, conf = 'BGGR', 0.0                                                                        # saturated or empty frame: assume the usual lattice (flagged by BAYERCNF)
    chan = {'R': 0, 'G': 1, 'B': 2}; mosaic = np.zeros(rgb.shape[:2], np.uint16)
    for k, ch in enumerate(pat): mosaic[k // 2::2, k % 2::2] = rgb[k // 2::2, k % 2::2, chan[ch]]
    mosaic = np.clip(mosaic.astype(np.int32) + BLACK, 0, 65535).astype(np.uint16)
    t = tifffile.TiffFile(src).pages[0]; exif = t.tags[34665].value if 34665 in t.tags else {}; gps = t.tags[34853].value if 34853 in t.tags else {}
    h = fits.PrimaryHDU(mosaic); hd = h.header
    ex = exif.get('ExposureTime'); hd['EXPTIME'] = (float(ex[0]) / float(ex[1])) if isinstance(ex, tuple) and ex[1] else (float(ex) if ex else 0.0)
    hd['ISOSPEED'] = exif.get('ISOSpeed', 0)
    m = re.search(r'_(\d{13})\.', os.path.basename(src))
    when = datetime.fromtimestamp(int(m.group(1)) / 1000, timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] if m else str(exif.get('DateTimeOriginal', '')).replace(':', '-', 2).replace(' ', 'T')
    hd['DATE-OBS'] = when; hd['CREATOR'] = str(t.tags[305].value)[:60] if 305 in t.tags else 'Origin'; hd['BAYERPAT'] = pat; hd['BAYERCNF'] = (round(conf, 2), 'lattice detection contrast (0 = assumed)')
    hd['BLKLEVEL'] = (BLACK, 'black level re-added: the source TIFF had it removed'); hd['ORIGFILE'] = os.path.basename(src)
    try:
        la, lo = gps.get('GPSLatitude'), gps.get('GPSLongitude')
        if la and lo:
            f = lambda v: sum(float(x[0]) / float(x[1]) / 60 ** i for i, x in enumerate(v)); hd['LATITUDE'] = f(la); hd['LONGITUD'] = f(lo) * (-1 if gps.get('GPSLongitudeRef') == 'W' else 1)
    except Exception: pass
    hd['COMMENT'] = 'Re-mosaicked from a pre-debayered Origin TIFF: only the native sample at each pixel is kept.'
    h.writeto(out, overwrite=True, output_verify='silentfix')
    if compress:
        subprocess.run(['fpack', '-Y', out], check=True, capture_output=True)                                       # lossless Rice tile compression: out.fz; -Y overwrites an old .fz
        os.remove(out)
    return pat, conf

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('src'); ap.add_argument('out'); ap.add_argument('--compress', action='store_true'); ap.add_argument('--limit', type=int, default=0); a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    files = [a.src] if os.path.isfile(a.src) else sorted(glob.glob(os.path.join(a.src, '*.tif*')))
    n = 0
    for f in files:
        if a.limit and n >= a.limit: break
        dst = os.path.join(a.out, os.path.splitext(os.path.basename(f))[0] + '.fits')
        if os.path.exists(dst) or os.path.exists(dst + '.fz'): continue
        try: pat, conf = convert(f, dst, compress=a.compress); n += 1; print('%s %s %.2f' % (os.path.basename(f), pat, conf), flush=True)
        except Exception as e: print('FAILED', f, e, file=sys.stderr)

if __name__ == '__main__':
    main()
