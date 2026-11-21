#!/usr/bin/env python3
"""Gaia colour calibration of every saved live stack (tools/StackColour, LiveStacker::calibrateColour) and new previews.

    stack_colour.py [--stacks /mnt/fits-backup/claude/stacks] [--tool build/stackcolour/StackColour] [--only NAME ...] [--dry-run]

The match against Gaia starts from the pointing in the stack's colour.json; a stack saved before that was recorded is plate-solved first
(astrometry.net solve-field on its preview, which needs no position hint). Each stack's report is printed as it goes, and a summary at the end.
"""
import argparse, glob, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def solve_centre(png):
    """(ra, dec) in degrees of the field centre of a preview, or None"""
    with tempfile.TemporaryDirectory() as tmp:
        p = subprocess.run(["solve-field", png, "--dir", tmp, "--scale-units", "arcsecperpix", "--scale-low", "2.7", "--scale-high", "3.2",
                            "--downsample", "2", "--no-plots", "--overwrite", "--cpulimit", "60", "--new-fits", "none", "--corr", "none",
                            "--rdls", "none", "--match", "none", "--solved", "none", "--index-xyls", "none", "--axy", "none", "--temp-axy"],
                           capture_output=True, text=True, timeout=180)
    m = re.search(r"Field center: \(RA,Dec\) = \(([-\d.]+), ([-\d.]+)\) deg", p.stdout)
    return (float(m.group(1)), float(m.group(2))) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stacks", default="/mnt/fits-backup/claude/stacks")
    ap.add_argument("--tool", default=os.path.join(HERE, "..", "build", "stackcolour", "StackColour"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    rows = []
    for st in sorted(glob.glob(os.path.join(a.stacks, "*", "state.bin"))):
        d = os.path.dirname(st); name = os.path.basename(d)
        if a.only and name not in a.only: continue
        args = [a.tool, d]
        cj = os.path.join(d, "colour.json")
        have_pointing = os.path.exists(cj) and "pointing" in json.load(open(cj))
        if not have_pointing:
            png = os.path.join(d, "preview_starwb.png") if os.path.exists(os.path.join(d, "preview_starwb.png")) else os.path.join(d, "preview.png")
            c = solve_centre(png) if os.path.exists(png) else None
            if not c: rows.append((name, None, "preview did not plate-solve")); print(f"{name}: preview did not plate-solve", flush=True); continue
            args += ["--ra", "%.5f" % c[0], "--dec", "%.5f" % c[1]]
        if a.dry_run: args.append("--dry-run")
        p = subprocess.run(args, capture_output=True, text=True)
        try: r = json.loads(p.stdout.strip().splitlines()[-1])
        except Exception: r = {"ok": False, "reason": (p.stderr or p.stdout).strip()[:200]}
        rows.append((name, r, r.get("reason")))
        if r.get("ok"):
            print(f"{name}: {r['filter']} {r['frames']} frames, {r['stars']} stars, gains R {r['gain_r']:.3f} B {r['gain_b']:.3f} (camera R {r['camera_gain_r']:.3f} B {r['camera_gain_b']:.3f})", flush=True)
        else:
            print(f"{name}: failed: {r.get('reason')}", flush=True)
    ok = [x for x in rows if x[1] and x[1].get("ok")]
    print(f"\n{len(ok)} of {len(rows)} stacks calibrated" + (" (dry run, nothing written)" if a.dry_run else ""))
    for n, r, why in rows:
        if not (r and r.get("ok")): print(f"  not calibrated: {n}: {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
