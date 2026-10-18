"""Download survey cutouts of the stack's field on exactly its pixel grid (CDS hips2fits), as references for match_survey.py.

    fetch_survey.py --wcs wcs.json --dss2 dss2.png --ps1 ps1.png
The files are untrusted downloads: read them with `python3 -I`.
"""
import argparse, json, subprocess, urllib.parse
HIPS = {'dss2': 'CDS/P/DSS2/color', 'ps1': 'CDS/P/PanSTARRS/DR1/color-z-zg-g'}
KEYS = ['NAXIS1', 'NAXIS2', 'CTYPE1', 'CTYPE2', 'CRVAL1', 'CRVAL2', 'CRPIX1', 'CRPIX2', 'CD1_1', 'CD1_2', 'CD2_1', 'CD2_2', 'RADESYS']
ap = argparse.ArgumentParser(); ap.add_argument('--wcs', required=True)
for k in HIPS: ap.add_argument('--' + k)
a = ap.parse_args(); w = json.load(open(a.wcs)); q = urllib.parse.quote(json.dumps({k: w[k] for k in KEYS}))
for k, hips in HIPS.items():
    out = getattr(a, k)
    if not out: continue
    r = subprocess.run(['curl', '-sf', '-m', '300', '-o', out, '-w', '%{http_code} %{size_download}', 'https://alasky.cds.unistra.fr/hips-image-services/hips2fits?hips=%s&wcs=%s&format=png' % (hips, q)], capture_output=True, text=True)
    print(k, hips, r.stdout or r.stderr)
    if r.returncode: raise SystemExit('download of %s failed' % k)
