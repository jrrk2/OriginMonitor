#!/usr/bin/env python3
"""Fetch an asteroid's or comet's SPK file from JPL Horizons, for OriginHeadless body / goto-body / track.

    tools/fetch_spk.py 524522 [--name Zoozve] [--days 60] [--start YYYY-MM-DD] [--data /mnt/fits-backup/claude]

The first argument is anything Horizons resolves to one small body: a number ("524522", "99942" for Apophis), a provisional designation
("DES=2005 JE46") or a comet ("DES=C/2023 A3;CAP"). Writes DATA/spk/<name>.bsp and DATA/spk/<name>.json ({name, naif_id, file, start, stop, fetched});
OriginHeadless loads every DATA/spk/*.json at start, so `OriginHeadless body <name>` then works like a planet. The orbit is Horizons' current
solution; fetch again before a session if the object was only recently discovered (its orbit improves quickly).
"""
import argparse, base64, datetime as dt, json, os, re, sys, urllib.parse, urllib.request


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('object'); ap.add_argument('--name'); ap.add_argument('--days', type=int, default=60); ap.add_argument('--start')
    ap.add_argument('--data', default='/mnt/fits-backup/claude')
    a = ap.parse_args()
    start = dt.date.fromisoformat(a.start) if a.start else dt.date.today() - dt.timedelta(days=1)
    stop = start + dt.timedelta(days=a.days)
    obj = a.object if a.object.endswith(';') else a.object + ';'
    q = {'format': 'json', 'COMMAND': "'%s'" % obj, 'EPHEM_TYPE': 'SPK', 'OBJ_DATA': 'YES', 'START_TIME': "'%s'" % start, 'STOP_TIME': "'%s'" % stop}
    j = json.load(urllib.request.urlopen('https://ssd.jpl.nasa.gov/api/horizons.api?' + urllib.parse.urlencode(q), timeout=120))
    if 'spk' not in j: sys.exit('Horizons gave no SPK for %r:\n%s' % (a.object, j.get('result', j.get('error', ''))[:1500]))
    naif = int(j['spk_file_id'])
    m = re.search(r'JPL/HORIZONS\s+(.+?)\s{2,}', j.get('result', ''))
    name = a.name or (m.group(1).strip() if m else a.object.strip(';'))
    safe = re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_')
    d = os.path.join(a.data, 'spk'); os.makedirs(d, exist_ok=True)
    bsp = os.path.join(d, safe + '.bsp')
    with open(bsp, 'wb') as f: f.write(base64.b64decode(j['spk']))
    meta = {'name': name, 'naif_id': naif, 'file': bsp, 'start': start.isoformat(), 'stop': stop.isoformat(), 'fetched': dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
            'query': a.object}
    json.dump(meta, open(os.path.join(d, safe + '.json'), 'w'), indent=1)
    print('%s: NAIF id %d, %s to %s, %d bytes -> %s' % (name, naif, start, stop, os.path.getsize(bsp), bsp))


if __name__ == '__main__':
    main()
