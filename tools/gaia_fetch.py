#!/usr/bin/env python3
"""Fetch the Gaia DR3 stars the live stacker's colour calibration needs, straight from ESA's archive, into a local folder.

    gaia_fetch.py [--out /mnt/fits-backup/gaia/dr3_g14] [--gmax 14] [--order 3]

One synchronous TAP query per HEALPix pixel of --order (768 pixels at order 3): Gaia's source_id carries the order-12 HEALPix index of the
source (source_id // 2^35), so a pixel at a lower order is a contiguous source_id range and needs no geometry in the query. The synchronous
service returns at most 50000 rows (84 order-3 pixels along the Milky Way reach that at G < 14), so a query that comes back full is split into
its four sub-pixels, recursively, and the parts are written together. A file holding exactly 50000 stars is a truncated one from before that
and is fetched again. Each pixel is written
as hp<order>_<pixel>.csv.gz with source_id, ra, dec (ICRS, epoch 2016.0), pmra, pmdec (mas/yr), phot_g_mean_mag and bp_rp. A pixel already on
disk is skipped, so the run can be stopped and started again; a failed query is retried and then reported and left for the next run.
Gaia data: ESA/Gaia/DPAC (https://www.cosmos.esa.int/gaia), which asks to be credited wherever the data are used.
"""
import argparse, gzip, os, sys, time, urllib.parse, urllib.request

TAP = "https://gea.esac.esa.int/tap-server/tap/sync"
COLUMNS = "source_id, ra, dec, pmra, pmdec, phot_g_mean_mag, bp_rp"
SYNC_LIMIT = 50000


def query(adql, timeout=600):
    data = urllib.parse.urlencode({"REQUEST": "doQuery", "LANG": "ADQL", "FORMAT": "csv", "QUERY": adql}).encode()
    with urllib.request.urlopen(urllib.request.Request(TAP, data=data), timeout=timeout) as r:
        body = r.read()
    if not body.startswith(b"source_id,"): raise RuntimeError("not a CSV answer: " + body[:200].decode("latin-1"))
    return body


def fetch_pixel(order, p, gmax):
    """the CSV rows (without header) of one HEALPix pixel, split into sub-pixels while a query comes back at the row limit"""
    width = 4 ** (12 - order) * 2 ** 35
    body = query("SELECT %s FROM gaiadr3.gaia_source WHERE source_id >= %d AND source_id < %d AND phot_g_mean_mag < %g" % (COLUMNS, p * width, (p + 1) * width, gmax))
    rows = body.split(b"\n", 1)[1]
    if rows.count(b"\n") < SYNC_LIMIT or order >= 12: return rows
    return b"".join(fetch_pixel(order + 1, 4 * p + k, gmax) for k in range(4))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="/mnt/fits-backup/gaia/dr3_g14")
    ap.add_argument("--gmax", type=float, default=14.0, help="faintest G magnitude kept")
    ap.add_argument("--order", type=int, default=3, help="HEALPix order of the chunks (12 x 4^order pixels)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    npix = 12 * 4 ** a.order
    done = rows = 0; failed = []; t0 = time.time()
    for p in range(npix):
        path = os.path.join(a.out, "hp%d_%04d.csv.gz" % (a.order, p))
        if os.path.exists(path):
            with gzip.open(path, "rb") as f: n = f.read().count(b"\n") - 1
            if n != SYNC_LIMIT: done += 1; continue
        for attempt in range(4):
            try:
                body = COLUMNS.replace(" ", "").encode() + b"\n" + fetch_pixel(a.order, p, a.gmax)
                tmp = path + ".tmp"
                with gzip.open(tmp, "wb") as f: f.write(body)
                os.replace(tmp, path)
                n = body.count(b"\n") - 1; rows += n; done += 1
                print("pixel %d/%d: %d stars (%.0f s elapsed)" % (p + 1, npix, n, time.time() - t0), flush=True)
                break
            except Exception as e:
                print("pixel %d: attempt %d failed: %s" % (p, attempt + 1, e), file=sys.stderr, flush=True)
                time.sleep(10 * (attempt + 1))
        else:
            failed.append(p)
    print("done: %d of %d pixels on disk, %d stars fetched this run, %d failed %s, %.0f s" % (done, npix, rows, len(failed), failed[:20], time.time() - t0), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
