#!/usr/bin/env python3
"""Export the live stacker's saved state (stacks/<target>/state.bin, format LST1 or LST2, see LiveStacker::save) to a linear RGB FITS: the weighted mean sum(w x)/sum(w) per channel, in the reference frame's
geometry at half resolution (2 x the sensor pixel, about 2.96 arcsec). Pixels no frame covered are 0. Prints the header fields.

    export_livestack.py /mnt/fits-backup/claude/stacks/M1 out.fits
"""
import struct, sys
import numpy as np
from astropy.io import fits

def export(src, dst):
    b = open(src, 'rb').read()
    magic, w, h, n = struct.unpack_from('<Iiii', b, 0); assert magic in (0x4C535431, 0x4C535432), 'not a live stack state file'
    o = 16; ref_iso = 800                                                   # LST2 (2026-10-28) adds the reference ISO after the frame count; LST1 stacks were ISO 800
    if magic == 0x4C535432: (ref_iso,) = struct.unpack_from('<i', b, o); o += 4
    tot, ref_exp, best_raw, wbr, wbb = struct.unpack_from('<5d', b, o); (ns,) = struct.unpack_from('<i', b, o + 40); pos = o + 44 + ns * 25   # 6 floats + 1 byte per reference star
    arr = np.frombuffer(b, dtype='<f4', count=w * h * 9, offset=pos).reshape(3, h * w, 3)
    sx, sw = arr[:, :, 0], arr[:, :, 2]
    mean = np.where(sw > 0, sx / np.maximum(sw, 1e-12), 0).reshape(3, h, w)
    scale = 65535.0 if mean.max() > 2 else 1.0
    hd = fits.Header(); hd['STACKFRM'] = n; hd['STACKEXP'] = tot; hd['REFEXP'] = ref_exp; hd['WBR'] = wbr; hd['WBB'] = wbb; hd['REFISO'] = ref_iso; hd['COMMENT'] = 'weighted mean of the live stack, half resolution, linear'
    fits.PrimaryHDU((mean / scale).astype(np.float32), header=hd).writeto(dst, overwrite=True)
    return {'width': w, 'height': h, 'frames': n, 'total_exposure_s': tot, 'ref_exposure_s': ref_exp, 'ref_iso': ref_iso, 'wb_r': wbr, 'wb_b': wbb, 'reference_stars': ns, 'scale_divisor': scale}

if __name__ == '__main__':
    import json; print(json.dumps(export(sys.argv[1], sys.argv[2])))
