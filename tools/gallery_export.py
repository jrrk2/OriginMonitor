#!/usr/bin/env python3
"""Export stacks as 16-bit PNGs for sharing (e.g. Facebook), cropped so that field rotation leaves no wedges, with a caption file.

    tools/gallery_export.py OUT_DIR SRC [SRC ...] [--long 2048] [--cover 0.7]

SRC is a live-stack or restack directory holding state.bin (stacks/<target>, finals/<name>_<filter>), or a tools/fullres_stack.py output holding RGB.fits
(and coverage.fits, or a sibling stack's). For each:
  * coverage: the per-pixel weight (state.bin) or frame count (coverage.fits); the crop is the LARGEST axis-aligned rectangle in which every pixel has at
    least --cover x the peak (the live preview only asks each border to be 98% covered, which lets the wedge corners of a rotated field through, e.g. the
    Rosette, 13 Nov 2026)
  * live/restack stacks: the stacker's own preview.png (its colour and stretch, as the gallery and the live view), placed in the frame by reproducing
    LiveStacker::preview()'s crop, then trimmed to that rectangle
  * full-resolution stacks: each channel's sky set to zero, R and B scaled so the median star is white, one asinh stretch
  * long side at most --long px (never enlarged)
Writes OUT_DIR/<slug>.png (16-bit RGB) and appends a line per image to OUT_DIR/captions.txt (target, exposure, frames, nights, filter, scale).
"""
import argparse, json, os, struct, sys
import numpy as np


def read_state(path):
    b = open(path, 'rb').read()
    magic, w, h, n = struct.unpack_from('<Iiii', b, 0)
    o = 16
    if magic != 0x4C535431: o += 4
    if magic == 0x4C535433: (fl,) = struct.unpack_from('<i', b, o); o += 4 + fl
    tot = struct.unpack_from('<d', b, o)[0]; (ns,) = struct.unpack_from('<i', b, o + 40); pos = o + 44 + ns * 25
    arr = np.frombuffer(b, dtype='<f4', count=w * h * 9, offset=pos).reshape(3, h * w, 3)
    sx, sw = arr[:, :, 0], arr[:, :, 2]
    mean = np.where(sw > 0, sx / np.maximum(sw, 1e-12), 0).reshape(3, h, w)
    return np.moveaxis(mean, 0, 2).astype(np.float32), sw[1].reshape(h, w).astype(np.float32), {'frames': n, 'exposure_s': tot, 'scale_arcsec': 2.96}


def read_fullres(d):
    from astropy.io import fits
    rgb = np.moveaxis(fits.getdata(os.path.join(d, 'RGB.fits')).astype(np.float32), 0, 2)
    cov = None
    for cand in (os.path.join(d, 'coverage.fits'), os.path.join(os.path.dirname(d), 'M1_nebula', 'coverage.fits')):
        if os.path.exists(cand): cov = fits.getdata(cand).astype(np.float32); break
    if cov is None: cov = np.isfinite(rgb[..., 0]).astype(np.float32)
    info = {'scale_arcsec': 1.478}
    try:
        m = json.load(open(os.path.join(d, 'manifest.json')))
        if 'combined' in m: info['combined'] = [c['stack'] for c in m['combined']]
        else: info.update(frames=m.get('frames_stacked'), exposure_s=m.get('exposure_s'))
    except (OSError, ValueError): pass
    return np.nan_to_num(rgb), cov, info


def largest_rectangle(mask):
    """largest all-True axis-aligned rectangle (x0, y0, x1, y1): histogram method on each row"""
    h, w = mask.shape; heights = np.zeros(w, int); best = (0, 0, 0, 0, 0)
    for y in range(h):
        heights = np.where(mask[y], heights + 1, 0)
        stack = []
        for x in range(w + 1):
            cur = heights[x] if x < w else 0
            start = x
            while stack and stack[-1][1] >= cur:
                sx, sh = stack.pop()
                area = sh * (x - sx)
                if area > best[0]: best = (area, sx, y - sh + 1, x, y + 1)
                start = sx
            stack.append((start, cur))
    return best[1:]


def live_preview_rect(sw1):
    """the rectangle LiveStacker::preview() crops to (same rule: shrink until every border is 98% covered by 60% of the peak weight), so that its
    preview.png can be placed in the full frame"""
    h, w = sw1.shape; cov = sw1 >= 0.6 * sw1.max()
    x0, y0, x1, y1 = 0, 0, w, h
    for _ in range(4000):
        if x1 - x0 <= 32 or y1 - y0 <= 32: break
        t, bt = cov[y0, x0:x1].mean(), cov[y1 - 1, x0:x1].mean(); l, r = cov[y0:y1, x0].mean(), cov[y0:y1, x1 - 1].mean()
        worst = min(t, bt, l, r)
        if worst >= 0.98: break
        if worst == t: y0 += 1
        elif worst == bt: y1 -= 1
        elif worst == l: x0 += 1
        else: x1 -= 1
    return x0, y0, x1, y1


def coverage_rect(cov, cover):
    import cv2
    cov = cv2.morphologyEx(cov.astype(np.float32), cv2.MORPH_CLOSE, np.ones((31, 31), np.uint8))   # fill the holes the stacker leaves at saturated star cores
    ref = np.percentile(cov[cov > 0], 90)                            # the typical well-covered level (the single peak can be a small multi-night overlap)
    m = cov >= cover * ref
    f = 4
    hc, wc = m.shape[0] // f, m.shape[1] // f
    mc = m[:hc * f, :wc * f].reshape(hc, f, wc, f).all(axis=(1, 3))
    return tuple(v * f for v in largest_rectangle(mc))


def from_preview(src, sw1, cover, long_side):
    """the stacker's own preview.png (its colour and stretch, as the gallery and the live view), cropped to the fully covered rectangle"""
    import cv2
    im = cv2.imread(os.path.join(src, 'preview.png'), cv2.IMREAD_UNCHANGED)[..., ::-1].astype(np.float32) / 65535.0
    px0, py0, px1, py1 = live_preview_rect(sw1)
    if (px1 - px0, py1 - py0) != (im.shape[1], im.shape[0]): raise ValueError(f'preview {im.shape[1]}x{im.shape[0]} does not match the stacker crop {px1 - px0}x{py1 - py0}')
    x0, y0, x1, y1 = coverage_rect(sw1, cover)
    x0, y0, x1, y1 = max(x0, px0), max(y0, py0), min(x1, px1), min(y1, py1)
    out = im[y0 - py0:y1 - py0, x0 - px0:x1 - px0]
    if max(out.shape[:2]) > long_side:
        s = long_side / max(out.shape[:2]); out = cv2.resize(out, (int(out.shape[1] * s), int(out.shape[0] * s)), interpolation=cv2.INTER_AREA)
    return out, (x0, y0, x1, y1)


def render_calibrated(src, cover=0.85, long_side=4096, hi_pct=99.7):
    """from the linear stack with the Gaia gains of tools/gaia_colour.py (src/gaia_colour.json): channels scaled so a solar-type star is white, the
    wedge-free rectangle, and the crossfade animation's stretch (sky neutral, filter colours kept, gentle asinh). None when there is no calibration."""
    import cv2, crossfade
    gj = os.path.join(src, 'gaia_colour.json')
    if not os.path.exists(gj): return None
    g = json.load(open(gj))['gains']
    if os.path.exists(os.path.join(src, 'state.bin')): rgb, cov, _ = read_state(os.path.join(src, 'state.bin'))
    else: rgb, cov, _ = read_fullres(src)
    x0, y0, x1, y1 = coverage_rect(cov, cover)
    soft = 25.0 if os.path.exists(os.path.join(src, 'state.bin')) else 40.0      # half-resolution stacks: a gentler stretch (less grain in empty fields)
    out = crossfade.stretch(rgb * np.array([g['R'], g['G'], g['B']], np.float32), (x0, y0, x1, y1), soft, True, hi_pct)     # flipped to sky orientation; hi_pct 99.97 for small bright targets (planetaries), 99.7 for field-filling nebulae
    if max(out.shape[:2]) > long_side:
        s = long_side / max(out.shape[:2]); out = cv2.resize(out, (int(out.shape[1] * s), int(out.shape[0] * s)), interpolation=cv2.INTER_AREA)
    return out, (x0, y0, x1, y1)


def finish(rgb, cov, cover, long_side):
    import cv2
    f = 4                                                             # search on a 4x coarser grid (a coarse cell counts only if all of it is covered)
    cov = cv2.morphologyEx(cov.astype(np.float32), cv2.MORPH_CLOSE, np.ones((31, 31), np.uint8))   # fill the holes the stacker leaves at saturated star cores
    ref = np.percentile(cov[cov > 0], 90)                            # the typical well-covered level (the single peak can be a small multi-night overlap)
    m = cov >= cover * ref
    hc, wc = m.shape[0] // f, m.shape[1] // f
    mc = m[:hc * f, :wc * f].reshape(hc, f, wc, f).all(axis=(1, 3))
    x0, y0, x1, y1 = (v * f for v in largest_rectangle(mc))
    c = rgb[y0:y1, x0:x1].copy()
    lum = c.mean(axis=2)
    dark = lum <= np.median(lum)
    for k in range(3): c[..., k] -= np.median(c[..., k][dark])         # sky to zero in every channel
    lum = c.mean(axis=2); sm = cv2.GaussianBlur(lum, (0, 0), 1.5)
    from scipy import ndimage
    pk = (sm == ndimage.maximum_filter(sm, 9)) & (sm > np.percentile(sm, 99.5)) & (sm < np.percentile(sm, 99.97))
    if pk.sum() > 20:                                                  # stars white: scale R and B to G at the star peaks
        g = np.median(c[..., 1][pk])
        for k in (0, 2):
            v = np.median(c[..., k][pk])
            if v > 0: c[..., k] *= g / v
    lum = c.mean(axis=2)
    d = lum[dark]; sd = 1.4826 * float(np.median(np.abs(d - np.median(d))))   # sky noise
    lo, hi = float(np.median(d)) - 1.5 * sd, float(np.percentile(lum, 99.95))
    soft = max((hi - lo) / 25.0, 1e-9)
    out = np.clip(np.arcsinh(np.clip(c - lo, 0, None) / soft) / np.arcsinh((hi - lo) / soft), 0, 1)[::-1]   # stored rows -> sky orientation
    if max(out.shape[:2]) > long_side:
        s = long_side / max(out.shape[:2]); out = cv2.resize(out, (int(out.shape[1] * s), int(out.shape[0] * s)), interpolation=cv2.INTER_AREA)
    return out, (x0, y0, x1, y1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out'); ap.add_argument('src', nargs='+'); ap.add_argument('--long', type=int, default=2048); ap.add_argument('--cover', type=float, default=0.85); ap.add_argument('--hi-pct', type=float, default=99.7, help='white point percentile of the smoothed luminance (99.97 for small bright targets)')
    ap.add_argument('--titles', help='JSON {src_basename: "Title"}'); a = ap.parse_args()
    import cv2
    os.makedirs(a.out, exist_ok=True)
    titles = json.load(open(a.titles)) if a.titles else {}
    cap = open(os.path.join(a.out, 'captions.txt'), 'a')
    for src in a.src:
        src = src.rstrip('/'); base = os.path.basename(src)
        cal = render_calibrated(src, a.cover, a.long, a.hi_pct)
        if cal is not None:
            out, box = cal
            info = read_state(os.path.join(src, 'state.bin'))[2] if os.path.exists(os.path.join(src, 'state.bin')) else read_fullres(src)[2]
        elif os.path.exists(os.path.join(src, 'state.bin')):
            rgb, cov, info = read_state(os.path.join(src, 'state.bin'))
            out, box = from_preview(src, cov, a.cover, a.long)
        else:
            rgb, cov, info = read_fullres(src)
            out, box = finish(rgb, cov, a.cover, a.long)
        slug = ''.join(ch if ch.isalnum() else '_' for ch in base).strip('_').lower()
        cv2.imwrite(os.path.join(a.out, slug + '.png'), (out[..., ::-1] * 65535).astype(np.uint16))
        man = {}
        try: man = json.load(open(os.path.join(src, 'manifest.json')))
        except (OSError, ValueError): pass
        nights = ', '.join(sorted(man.get('nights', {}))) if isinstance(man.get('nights'), dict) else ''
        st = man.get('stack', {})
        frames = st.get('frames', info.get('frames')); expo = st.get('total_exposure_s', info.get('exposure_s'))
        filt = man.get('filter', '')
        line = f"{slug}.png: {titles.get(base, base)}" + (f", {expo / 60:.0f} min" if expo else '') + (f" in {frames} frames" if frames else '') + \
               (f", nights {nights}" if nights else '') + (f", filter {filt}" if filt else '') + f"; {info['scale_arcsec']}\"/px; crop {box}"
        cap.write(line + '\n'); print(line, flush=True)


if __name__ == '__main__':
    main()
