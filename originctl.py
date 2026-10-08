#!/usr/bin/env python3
"""Command-line client for the CelestronOriginMonitor control server (ControlServer.cpp).

Start the server (no GUI):
    CelestronOriginMonitor --headless --connect 192.168.1.195 [--control-port 8765] [--control-raw]

Then, for example:
    originctl.py status
    originctl.py manual
    originctl.py set --exposure 0.002 --iso 200      # exposure in seconds (camera minimum 1 us)
    originctl.py snapshot --dir /tmp/frames
    originctl.py focus --history 10      # HFR of the last snapshots (lower is better)
    originctl.py focuser                      # position, limits, NeedAutoFocus
    originctl.py focuser-move 18800
    originctl.py mount                        # RA/Dec/Alt/Az, tracking, goto state
    originctl.py goto 1.78 61.23              # J2000 RA hours, Dec degrees
    originctl.py focus-sweep --span 600 --step 100 --frames 3

Every command prints one JSON object, so it is easy for scripts and AI agents to parse.
Only the Python standard library is needed; focus-sweep additionally uses numpy and astropy
(for FITS) or Pillow (for PNG).
"""
import argparse
import json
import os
import socket
import sys


def call(port, **req):
    with socket.create_connection(("127.0.0.1", port), timeout=120) as s:
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf)


def sharpness(path):
    """Median half-flux radius (HFR, half-res pixels) of unsaturated stars; smaller = better focused.

    Works on a FITS (Bayer mosaic, binned 2x2) or PNG star field. Returns None if too few usable stars."""
    import numpy as np
    from scipy import ndimage as ndi
    if path.lower().endswith(".fits"):
        from astropy.io import fits
        raw = fits.getdata(path).astype(np.float32)
        sat = ndi.maximum_filter(raw, 5)[::2, ::2] >= 60000
        d = raw[0::2, 0::2] + raw[0::2, 1::2] + raw[1::2, 0::2] + raw[1::2, 1::2] - 4 * 4985.0
    else:
        from PIL import Image
        d = np.asarray(Image.open(path).convert("L"), dtype=np.float32)
        sat = ndi.maximum_filter(d, 5) >= 250
    bg = ndi.median_filter(d[::4, ::4], size=15)
    bg = ndi.zoom(bg, 4, order=1)[:d.shape[0], :d.shape[1]]
    d = d - bg
    sig = 1.4826 * np.median(np.abs(d - np.median(d)))
    sm = ndi.gaussian_filter(d, 1.2)
    peaks = np.argwhere((sm == ndi.maximum_filter(sm, 11)) & (sm > 10 * sig) & ~sat)
    peaks = [p for p in peaks if 20 < p[0] < d.shape[0] - 20 and 20 < p[1] < d.shape[1] - 20]
    peaks.sort(key=lambda p: -sm[p[0], p[1]])
    yy, xx = np.mgrid[-12:13, -12:13]
    rr = np.hypot(yy, xx)
    hfrs = []
    for y, x in peaks[:60]:
        c = d[y - 12:y + 13, x - 12:x + 13]
        c = np.clip(c, 0, None)
        if c[rr <= 12].sum() <= 0:
            continue
        cy = (c * yy).sum() / c.sum(); cx = (c * xx).sum() / c.sum()
        r = np.hypot(yy - cy, xx - cx)
        order = np.argsort(r[rr <= 12]); flux = np.cumsum(c[rr <= 12][order])
        hfr = float(r[rr <= 12][order][np.searchsorted(flux, flux[-1] / 2)])
        hfrs.append(hfr)
    if len(hfrs) < 5:
        return {"hfr": None, "n": len(hfrs)}
    return {"hfr": float(np.median(hfrs)), "n": len(hfrs), "sigma": float(sig)}


def focus_sweep(port, a):
    st = call(port, cmd="focuser_status")
    if not st.get("ok"):
        return st
    start = st["focuser"]["Position"]
    positions = list(range(start - a.span, start + a.span + 1, a.step))
    results = []
    os.makedirs(a.dir, exist_ok=True)
    for p in positions:
        mv = call(port, cmd="focuser_move", position=p, max_step=a.span + a.step + 100)
        if not mv.get("ok"):
            results.append({"position": p, "error": mv})
            break
        score = []
        for _ in range(a.frames):
            sn = call(port, cmd="snapshot", dir=a.dir)
            if sn.get("ok"):
                m = sharpness(sn["path"])
                if m and m["hfr"] is not None:
                    score.append(m)
        if score:
            hfr = sorted(m["hfr"] for m in score)[len(score) // 2]
            results.append({"position": p, "hfr": round(hfr, 3), "stars": int(sum(m["n"] for m in score) / len(score)), "frames": len(score)})
        else:
            results.append({"position": p, "hfr": None, "note": "too few measurable stars (badly defocused?)"})
        print(json.dumps(results[-1]), file=sys.stderr, flush=True)
    good = [r for r in results if r.get("hfr") is not None]
    best = min(good, key=lambda r: r["hfr"]) if good else None
    final = start
    if a.apply and best:
        final = best["position"]
    back = call(port, cmd="focuser_move", position=final, max_step=a.span + a.step + 100)
    return {"ok": bool(good), "start": start, "best": best, "applied": bool(a.apply and best), "final": back, "results": results}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("ping", "status", "manual", "auto", "focuser", "focuser-settings", "mount", "quit"):
        sub.add_parser(name)
    c = sub.add_parser("connect"); c.add_argument("host")
    fo = sub.add_parser("focus"); fo.add_argument("--history", type=int, default=0, help="also return the newest N scores")
    s = sub.add_parser("set")
    s.add_argument("--exposure", type=float); s.add_argument("--iso", type=int)
    s.add_argument("--binning", type=int)
    n = sub.add_parser("snapshot"); n.add_argument("--dir"); n.add_argument("--timeout", type=int, default=30)
    g = sub.add_parser("goto"); g.add_argument("ra_hours", type=float); g.add_argument("dec_deg", type=float)
    m = sub.add_parser("focuser-move"); m.add_argument("position", type=int); m.add_argument("--max-step", type=int, default=3000)
    f = sub.add_parser("focus-sweep")
    f.add_argument("--span", type=int, default=600, help="steps either side of the current position")
    f.add_argument("--step", type=int, default=100)
    f.add_argument("--frames", type=int, default=3, help="snapshots per position (median sharpness)")
    f.add_argument("--dir", default="/tmp/originctl_sweep")
    f.add_argument("--apply", action="store_true", help="leave the focuser at the best position (default: restore the start)")
    r = sub.add_parser("send"); r.add_argument("command"); r.add_argument("destination"); r.add_argument("--params", default="{}")
    a = ap.parse_args()

    if a.cmd == "focus-sweep":
        out = focus_sweep(a.port, a)
    elif a.cmd == "set":
        out = call(a.port, cmd="set", **{k: v for k, v in vars(a).items() if k in ("exposure", "iso", "binning") and v is not None})
    elif a.cmd == "snapshot":
        out = call(a.port, cmd="snapshot", timeout=a.timeout, **({"dir": a.dir} if a.dir else {}))
    elif a.cmd == "focus":
        out = call(a.port, cmd="focus_status", history=a.history)
    elif a.cmd == "focuser":
        out = call(a.port, cmd="focuser_status")
    elif a.cmd == "focuser-settings":
        out = call(a.port, cmd="focuser_settings")
    elif a.cmd == "mount":
        out = call(a.port, cmd="mount_status")
    elif a.cmd == "goto":
        out = call(a.port, cmd="goto", ra_hours=a.ra_hours, dec_deg=a.dec_deg)
    elif a.cmd == "focuser-move":
        out = call(a.port, cmd="focuser_move", position=a.position, max_step=a.max_step)
    elif a.cmd == "connect":
        out = call(a.port, cmd="connect", host=a.host)
    elif a.cmd == "send":
        out = call(a.port, cmd="send", command=a.command, destination=a.destination, params=json.loads(a.params))
    else:
        out = call(a.port, cmd=a.cmd)
    print(json.dumps(out))
    return 0 if out.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
