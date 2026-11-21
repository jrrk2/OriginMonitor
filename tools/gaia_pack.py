#!/usr/bin/env python3
"""Pack the Gaia DR3 chunks fetched by gaia_fetch.py into the one binary file the live stacker's colour calibration reads (GaiaCatalog.hpp).

    gaia_pack.py [--in /mnt/fits-backup/gaia/dr3_g14] [--out /mnt/fits-backup/gaia/gaia_dr3_g14.bin] [--gmax 14]

Layout (little-endian):
    header, 64 bytes: magic "GAIACAT1", uint32 zones (180: 1 degree of Dec each, from -90), uint32 bins (360: 1 degree of RA each),
                      float32 epoch (2016.0), float32 gmax, uint64 stars, 32 bytes zero
    index:            uint64 x (zones * bins + 1), the first star of each cell (zone-major), the last entry the star count
    stars, 16 bytes:  uint32 ra (1e-7 deg), int32 dec (1e-7 deg), int16 pmra, int16 pmdec (mas/yr, clamped to +-32767), int16 G (millimag),
                      int16 BP-RP (millimag, -32768 when Gaia has none)
Stars within a cell are sorted brightest first. Gaia data: ESA/Gaia/DPAC (https://www.cosmos.esa.int/gaia).
"""
import argparse, glob, gzip, os, struct, sys
import numpy as np

ZONES, BINS = 180, 360
REC = np.dtype([("ra", "<u4"), ("dec", "<i4"), ("pmra", "<i2"), ("pmdec", "<i2"), ("g", "<i2"), ("bprp", "<i2")])


def read_chunk(path):
    with gzip.open(path, "rt") as f:
        next(f)
        cols = [[] for _ in range(6)]
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) < 7: continue
            for k in range(6): cols[k].append(p[k + 1] or "nan")
    return [np.array(c, dtype=np.float64) for c in cols]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", default="/mnt/fits-backup/gaia/dr3_g14")
    ap.add_argument("--out", default="/mnt/fits-backup/gaia/gaia_dr3_g14.bin")
    ap.add_argument("--gmax", type=float, default=14.0)
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.src, "hp*_*.csv.gz")))
    if not files: sys.exit("no chunks in " + a.src)
    parts = []
    for i, path in enumerate(files):
        ra, dec, pmra, pmdec, g, bprp = read_chunk(path)
        keep = np.isfinite(ra) & np.isfinite(dec) & np.isfinite(g) & (g < a.gmax)
        r = np.zeros(keep.sum(), REC)
        r["ra"] = np.clip(np.round(ra[keep] % 360.0 * 1e7), 0, 3599999999).astype(np.uint32)
        r["dec"] = np.round(dec[keep] * 1e7).astype(np.int32)
        r["pmra"] = np.clip(np.round(np.nan_to_num(pmra[keep])), -32767, 32767).astype(np.int16)
        r["pmdec"] = np.clip(np.round(np.nan_to_num(pmdec[keep])), -32767, 32767).astype(np.int16)
        r["g"] = np.round(g[keep] * 1000).astype(np.int16)
        c = bprp[keep]
        r["bprp"] = np.where(np.isfinite(c), np.clip(np.round(np.nan_to_num(c) * 1000), -32767, 32767), -32768).astype(np.int16)
        parts.append(r)
        if (i + 1) % 100 == 0: print("%d/%d chunks read" % (i + 1, len(files)), flush=True)
    s = np.concatenate(parts); del parts
    zone = np.clip(np.floor(s["dec"] / 1e7 + 90).astype(np.int64), 0, ZONES - 1)
    rbin = np.clip((s["ra"] // 10000000).astype(np.int64), 0, BINS - 1)
    cell = zone * BINS + rbin
    order = np.lexsort((s["g"], cell))
    s, cell = s[order], cell[order]
    index = np.searchsorted(cell, np.arange(ZONES * BINS + 1)).astype("<u8")
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"GAIACAT1" + struct.pack("<IIffQ", ZONES, BINS, 2016.0, a.gmax, len(s)) + b"\0" * 32)
        f.write(index.tobytes()); f.write(s.tobytes())
    os.replace(tmp, a.out)
    print("%s: %d stars from %d chunks, %.0f MB" % (a.out, len(s), len(files), os.path.getsize(a.out) / 1e6))


if __name__ == "__main__":
    main()
