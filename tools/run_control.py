#!/usr/bin/env python3
"""Talk to the unattended supervisor (OriginHeadless run) through its files in the data directory, as OriginWatch's Tasks tab does.

  tools/run_control.py status                 run_state.json and autorun_state.json in short; exit 0 when a run is alive
  tools/run_control.py pid                    pid of the live run (nothing, exit 1, when none)
  tools/run_control.py send ACTION [k=v ...]  write a request to run_control.json: stop [park=false], replan, end_override,
                                              override name=M31 minutes=60 (or ra_hours=.. dec_deg=..); the run acts within about 20 s
Data directory: $ORIGIN_DATA_DIR, else /mnt/fits-backup/claude (as DataDir.hpp).
"""
import json, os, sys, time
from datetime import datetime, timezone

DIR = os.environ.get("ORIGIN_DATA_DIR", "/mnt/fits-backup/claude")


def read(name):
    try:
        with open(os.path.join(DIR, name)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def alive(pid):
    try:
        pid = int(pid or 0)
        if pid <= 0:
            return False
        os.kill(pid, 0)
        return True
    except (OSError, ValueError, TypeError):
        return False


def run_pid():
    st = read("run_state.json")
    return int(st["pid"]) if alive(st.get("pid")) else 0


def value(s):
    if s in ("true", "false"):
        return s == "true"
    try:
        return float(s) if "." in s else int(s)
    except ValueError:
        return s


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd = argv[1]
    if cmd == "pid":
        pid = run_pid()
        if pid:
            print(pid)
        return 0 if pid else 1
    if cmd == "status":
        st, pid = read("run_state.json"), run_pid()
        cur = st.get("current") or {}
        if pid:
            print(f"run: alive (pid {pid}), night {st.get('night')}, phase {st.get('phase')}, updated {st.get('updated')}")
            print(f"     target {cur.get('name', '-')}" + (" (override)" if cur.get("override") else "") + f", block {st.get('block')}, log {st.get('log')}")
        else:
            print(f"run: not running" + (f" (last: night {st.get('night')}, phase {st.get('phase')})" if st else ""))
        auto = read("autorun_state.json")
        if auto:
            night = max(auto)
            a = auto[night]
            print(f"autorun: night {night}, launches {a.get('launches', 0)}, last launch {a.get('launched', '-')}")
        off = os.path.expanduser("~/.config/origin/autorun.off")
        print("autorun: DISABLED (" + off + ")" if os.path.exists(off) else "autorun: enabled (cron starts a run at dusk when the telescope answers)")
        return 0 if pid else 1
    if cmd == "send":
        if len(argv) < 3:
            print("send needs an action: stop, replan, end_override, override", file=sys.stderr)
            return 2
        if not run_pid():
            print("no unattended run is alive; nothing to send to", file=sys.stderr)
            return 1
        req = {"action": argv[2]}
        for kv in argv[3:]:
            k, _, v = kv.partition("=")
            req[k] = value(v)
        last = max(int(read("run_control.json").get("id", 0) or 0), int(read("run_state.json").get("control_done", 0) or 0))
        req.update({"id": last + 1, "sent": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "from": "Makefile"})
        tmp = os.path.join(DIR, "run_control.json.tmp")
        with open(tmp, "w") as f:
            json.dump(req, f, indent=1)
        os.replace(tmp, os.path.join(DIR, "run_control.json"))
        print(f"request {req['id']} sent: {json.dumps(req)}; the run acts on it within about 20 s")
        return 0
    print(f"unknown command {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
