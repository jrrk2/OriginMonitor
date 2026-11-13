#!/usr/bin/env python3
"""A video of a tracked asteroid, held still at the centre while the stars drift past, from a hostalign track capture folder.

    tools/asteroid_video.py DIR [--fps 12] [--bin 2] [--avg 1] [--crop 0] [--out DIR/video.mp4]

DIR is <data>/hostalign/<label>_<time>/ (the snapshots and frames.jsonl, which gives each frame's asteroid RA/Dec from the ephemeris). The frame headers carry
the firmware's (fake-initialised) RA/Dec, which may be far off, so:
  * one reference frame (the middle one) is plate-solved with solve-field, hinted by the asteroid's position;
  * every frame is registered to the reference by its stars (tools/fullres_stack.py: GRBG debayer, luminance, astroalign + a similarity fit), which also
    undoes the alt-az field rotation;
  * the asteroid's pixel in the reference at each frame's mid-exposure (ephemeris RA/Dec through the reference WCS) is moved to the output centre.
Writes DIR/video.mp4 (H.264), DIR/stack_on_asteroid.png (every frame, mean on the asteroid: star trails and a point), DIR/stack_on_stars.png (on the stars:
the asteroid's trail) and DIR/video_frames.jsonl (per frame: matched stars, fit rms, asteroid pixel). --avg N averages N consecutive frames per video frame.
"""
import argparse, json, os, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
from datetime import datetime, timedelta
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import fullres_stack as F                                         # noqa: E402


def mid_time(row):
    from astropy.io import fits
    h = fits.getheader(row['file'])
    t = datetime.fromisoformat(h['DATE-OBS'])                    # start of exposure (the header's local zone is +0000)
    return t + timedelta(seconds=float(h.get('EXPTIME', row.get('exposure', 0))) / 2)


def solve_ref(path, ra, dec, work):
    from astropy.io import fits
    from astropy.wcs import WCS
    y, _, _ = F.load(path)
    img = os.path.join(work, 'ref.fits')
    fits.PrimaryHDU(np.clip(y - np.nanmin(y), 0, 65535).astype(np.float32)).writeto(img, overwrite=True)
    cmd = ['solve-field', img, '--overwrite', '--no-plots', '--ra', str(ra), '--dec', str(dec), '--radius', '2', '--scale-units', 'arcsecperpix',
           '--scale-low', '1.35', '--scale-high', '1.6', '--downsample', '2', '--cpulimit', '120', '--dir', work, '--new-fits', 'none', '--corr', 'none',
           '--rdls', 'none', '--match', 'none', '--index-xyls', 'none', '--solved', 'none']
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    w = os.path.join(work, 'ref.wcs')
    if not os.path.exists(w): sys.exit('the reference frame did not plate-solve')
    return WCS(fits.getheader(w))


def measure(args):
    path, ref = args
    y, _, _ = F.load(path); s = F.stars(y); M, info = F.register(s, ref)
    if M is not None: info['M'] = M.tolist()
    info['file'] = path
    return info


def lum(path, binf):
    y, _, _ = F.load(path)
    return y


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir'); ap.add_argument('--fps', type=int, default=12); ap.add_argument('--bin', type=int, default=2); ap.add_argument('--avg', type=int, default=1)
    ap.add_argument('--crop', type=int, default=0, help='output size in full-resolution pixels around the asteroid (0: the whole frame)')
    ap.add_argument('--jobs', type=int, default=8); ap.add_argument('--out')
    a = ap.parse_args()
    import cv2
    rows = [json.loads(l) for l in open(os.path.join(a.dir, 'frames.jsonl')) if l.strip()]
    rows = [r for r in rows if os.path.exists(r['file'])]
    if len(rows) < 3: sys.exit('fewer than 3 frames')
    work = tempfile.mkdtemp(prefix='astvid_')
    ref_row = rows[len(rows) // 2]
    wcs = solve_ref(ref_row['file'], ref_row['target_ra_deg'], ref_row['target_dec_deg'], work)
    ry, _, _ = F.load(ref_row['file']); rstars = F.stars(ry)
    print(f"{len(rows)} frames; reference {os.path.basename(ref_row['file'])} solved, {len(rstars)} stars", flush=True)
    with ProcessPoolExecutor(a.jobs) as ex: infos = list(ex.map(measure, [(r['file'], rstars) for r in rows], chunksize=4))
    # frames far in time from the reference (the field drifts with the asteroid and rotates) may not match it directly: chain them through the nearest
    # registered frame, M_ref<-k = M_ref<-j . M_j<-k, working outward until nothing more joins
    ok = lambda i: 'M' in i and i.get('matched', 0) >= 30 and i.get('rms_px', 9) <= 0.8
    for _ in range(len(rows)):
        todo = [k for k, i in enumerate(infos) if not ok(i)]
        if not todo: break
        joined = 0
        for k in todo:
            js = sorted((j for j, i in enumerate(infos) if ok(i)), key=lambda j: abs(j - k))
            if not js: break
            j = js[0]
            ys, _, _ = F.load(rows[j]['file']); sj = F.stars(ys)
            yk, _, _ = F.load(rows[k]['file']); sk = F.stars(yk)
            Mjk, inf = F.register(sk, sj)
            if Mjk is None or inf.get('matched', 0) < 30 or inf.get('rms_px', 9) > 0.8: continue
            Mrj = np.vstack([np.array(infos[j]['M']), [0, 0, 1]]); Mc = (Mrj @ np.vstack([Mjk, [0, 0, 1]]))[:2]
            infos[k].update(inf); infos[k]['M'] = Mc.tolist(); infos[k]['chained_via'] = os.path.basename(rows[j]['file']); joined += 1
            infos[k]['rot_deg'] = round(float(np.degrees(np.arctan2(Mc[1, 0], Mc[0, 0]))), 3); infos[k]['scale'] = round(float(np.hypot(Mc[0, 0], Mc[1, 0])), 5)
            infos[k]['dx'] = round(float(Mc[0, 2]), 2); infos[k]['dy'] = round(float(Mc[1, 2]), 2)
        if not joined: break
    W, H = F.W, F.H
    size = (a.crop or W, a.crop or H) if a.crop else (W, H)
    cx, cy = size[0] / 2, size[1] / 2
    good = []
    for r, info in zip(rows, infos):
        if 'M' not in info or info.get('matched', 0) < 30 or info.get('rms_px', 9) > 0.8: info['used'] = False; continue
        # the asteroid's pixel in the reference: the ephemeris RA/Dec of this frame (frames.jsonl gives it for the frame's time)
        ax, ay = wcs.all_world2pix([[r['target_ra_deg'], r['target_dec_deg']]], 0)[0]
        info.update(used=True, ast_ref_px=[round(float(ax), 2), round(float(ay), 2)])
        M = np.vstack([np.array(info['M']), [0, 0, 1]])
        T = np.array([[1, 0, cx - ax], [0, 1, cy - ay], [0, 0, 1]])         # reference -> output (asteroid at the centre)
        good.append((r, (T @ M)[:2], np.array([[1, 0, cx - ax], [0, 1, cy - ay]])))
    print(f"registered {len(good)}/{len(rows)}", flush=True)
    with open(os.path.join(a.dir, 'video_frames.jsonl'), 'w') as f:
        for i in infos: f.write(json.dumps({k: v for k, v in i.items() if k != 'M'}) + '\n')
    # pass 1: warp each frame onto the asteroid; collect for the stretch and the stacks
    ast_sum = np.zeros((size[1], size[0]), np.float64); ast_n = np.zeros_like(ast_sum)
    ref_sum = np.zeros((H, W), np.float64); ref_n = np.zeros_like(ref_sum)
    frames = []
    for r, A, Tonly in good:
        y = lum(r['file'], a.bin)
        o = cv2.warpAffine(y, A, size, flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=np.nan)
        ok = np.isfinite(o); ast_sum += np.where(ok, o, 0); ast_n += ok
        M = np.array(dict((x['file'], x) for x in infos)[r['file']]['M'])
        s = cv2.warpAffine(y, M, (W, H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=np.nan)
        ok2 = np.isfinite(s); ref_sum += np.where(ok2, s, 0); ref_n += ok2
        small = cv2.resize(np.nan_to_num(o, nan=0.0), (size[0] // a.bin, size[1] // a.bin), interpolation=cv2.INTER_AREA)
        frames.append((r, small.astype(np.float32)))
    # one stretch for the whole video, from the median frame's statistics
    st = np.stack([f for _, f in frames[::max(1, len(frames) // 20)]])
    lo = float(np.percentile(st, 20)); hi = float(np.percentile(st, 99.7)); soft = (hi - lo) / 30
    def stretch(img): return (np.clip(np.arcsinh(np.clip(img - lo, 0, None) / soft) / np.arcsinh((hi - lo) / soft), 0, 1) * 255).astype(np.uint8)
    out = a.out or os.path.join(a.dir, 'video.mp4')
    fw, fh = (size[0] // a.bin) // 2 * 2, (size[1] // a.bin) // 2 * 2
    ff = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'gray', '-s', f'{fw}x{fh}', '-r', str(a.fps), '-i', '-',
                           '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18', out], stdin=subprocess.PIPE)
    ccx, ccy = int(cx / a.bin), int(cy / a.bin)
    for k in range(0, len(frames) - a.avg + 1):
        img = np.mean([f for _, f in frames[k:k + a.avg]], axis=0)
        g = stretch(img)[:fh, :fw].copy()
        cv2.circle(g, (ccx, ccy), 14, 255, 1)                         # a ring round the asteroid, clear of it
        r = frames[k][0]
        cv2.putText(g, '2005 JE46  ' + mid_time(r).strftime('%Y-%m-%d %H:%M:%S UT'), (12, fh - 14), cv2.FONT_HERSHEY_SIMPLEX, 0.6, 255, 1, cv2.LINE_AA)
        ff.stdin.write(g.tobytes())
    ff.stdin.close(); ff.wait()
    for name, s, n in (('stack_on_asteroid.png', ast_sum, ast_n), ('stack_on_stars.png', ref_sum, ref_n)):
        m = np.where(n > 0, s / np.maximum(n, 1), np.nan)
        m = cv2.resize(np.nan_to_num(m, nan=0.0).astype(np.float32), (m.shape[1] // a.bin, m.shape[0] // a.bin), interpolation=cv2.INTER_AREA)
        l2, h2 = np.percentile(m, 20), np.percentile(m, 99.8); s2 = (h2 - l2) / 60
        cv2.imwrite(os.path.join(a.dir, name), (np.clip(np.arcsinh(np.clip(m - l2, 0, None) / s2) / np.arcsinh((h2 - l2) / s2), 0, 1) * 255).astype(np.uint8))
    print(json.dumps({'video': out, 'frames': len(frames), 'video_frames': max(0, len(frames) - a.avg + 1), 'fps': a.fps, 'size': [fw, fh],
                      'stacks': [os.path.join(a.dir, 'stack_on_asteroid.png'), os.path.join(a.dir, 'stack_on_stars.png')]}), flush=True)
    subprocess.run(['rm', '-rf', work])


if __name__ == '__main__':
    main()
