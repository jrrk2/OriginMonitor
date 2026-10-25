"""Plate-solve a stack with the flow's own Gaia matcher and write the WCS as JSON (used for survey cutouts and for the background band).

    solve_wcs_cli.py --stack integrated.fits --ref-header RAW_FRAME --out wcs.json [--work DIR]
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assess, flow

ap = argparse.ArgumentParser(); ap.add_argument('--stack', required=True); ap.add_argument('--ref-header', required=True); ap.add_argument('--out', required=True); ap.add_argument('--work'); ap.add_argument('--bin', type=int, default=1)
a = ap.parse_args(); work = a.work or os.path.dirname(os.path.abspath(a.out)); os.makedirs(work, exist_ok=True)
d, _ = assess.load_stack(a.stack); w = flow.solve_wcs(d.mean(0), flow.header_of(a.ref_header), work, a.bin)
if w is None: sys.exit('astrometry failed')
w['NAXIS1'] = d.shape[2]; w['NAXIS2'] = d.shape[1]; json.dump(w, open(a.out, 'w'), indent=1)
