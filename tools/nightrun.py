#!/usr/bin/env python3
"""Helpers for an unattended night run: a supervising Claude session calls these between polls of originctl.py.

    tools/nightrun.py now                       # time, current target, how long each candidate stays observable, what the plan says
    tools/nightrun.py go M31 [--exposure 10 --iso 800]     # monitor site, target, slew, wait for centring/autofocus, re-send settings
    tools/nightrun.py park                      # save the stack and slew to Polaris (safe park)

`now` is read-only. `go` and `park` move the telescope: they are for a session the user has put in charge ("you have control").
The decision of when to leave a target stays with the supervisor (see OBSERVING.md); `now` only supplies the geometry: minutes until each
candidate sets behind the local horizon (tools/horizon.json), the sun/moon limits, and the planner's suggestion.
"""
import argparse, json, os, subprocess, sys, time
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__)); CTL = os.path.join(HERE, '..', 'originctl.py')
LAT, LON = 52.2054, 0.1218
POLARIS = (2.5303, 89.264)

def ctl(*args, check=True):
    p = subprocess.run([sys.executable, '-I', CTL, *map(str, args)], capture_output=True, text=True)
    try: out = json.loads(p.stdout.strip().splitlines()[-1])
    except Exception: out = {'ok': False, 'error': (p.stderr or p.stdout).strip().splitlines()[-1:] or 'no output'}
    if check and not out.get('ok'): sys.exit(f'originctl {" ".join(map(str, args))} failed: {out}')
    return out

def schedule(extra=()):
    day = (datetime.now() - timedelta(hours=12)).date()      # after midnight we are still in last evening's night
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'schedule.py'), '--json', '--date', str(day), *extra], capture_output=True, text=True)
    if p.returncode: sys.exit(p.stderr)
    return json.loads(p.stdout)

def parse(t): return datetime.strptime(t, '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=timezone.utc)

def cmd_now(a):
    S = schedule(); now = datetime.now(timezone.utc)
    ds, de = parse(S['dark_start']), parse(S['dark_end'])
    print(json.dumps({'now_utc': now.strftime('%H:%M'), 'dark': ds <= now <= de, 'dark_start': S['dark_start'], 'dark_end': S['dark_end'],
                      'minutes_to_dark': round((ds - now).total_seconds() / 60) if now < ds else 0,
                      'minutes_to_dawn': round((de - now).total_seconds() / 60) if now < de else 0, 'moon_illumination': S['moon_illumination']}))
    cur = ctl('target', check=False).get('target')
    print('current target:', cur or '(none set)')
    print('observable now (minutes left before leaving the window, priority, plan marker):')
    planned = next((p['key'] for p in S['plan'] if parse(p['start']) <= now <= parse(p['end']) + timedelta(minutes=5)), None)
    for t in sorted(S['targets'], key=lambda t: -t['priority']):
        if parse(t['window_start']) <= now <= parse(t['window_end']):
            left = (parse(t['window_end']) - now).total_seconds() / 60
            print(f'  {t["key"][:36]:36} {left:4.0f} min left  pri {t["priority"]:2d}  peak {t["peak"][11:16]}Z alt {t["peak_alt"]:.0f}' + ('   <- plan' if t['key'] == planned else '') + ('   <- current' if cur and t['key'] == cur else ''))

def lookup(name):
    S = schedule(['--min-block', '0']); n = name.lower()
    hits = [t for t in S['targets'] if n == t['key'].lower() or n == t['name'].lower()] or [t for t in S['targets'] if t['key'].lower().startswith(n + ' ') or n in t['name'].lower()]
    if not hits: sys.exit(f'{name!r} is not observable tonight (or unknown); see `now`')
    return hits[0]

def wait_idle(timeout=180):
    """after a goto the firmware centres and autofocuses (about 50 s); wait for TaskController to report idle"""
    t0 = time.time(); seen = None
    while time.time() - t0 < timeout:
        tc = ctl('status', check=False).get('last', {}).get('TaskController/GetStatus', {})
        seen = tc.get('State', seen)
        if seen and str(seen).upper() == 'IDLE' and time.time() - t0 > 5: return True
        time.sleep(3)
    print('warning: TaskController did not report IDLE; last state', seen, file=sys.stderr); return False

def too_early(a):
    """the user does not put the telescope out before this local time (20:00); no slewing until then"""
    hh, mm = map(int, a.not_before.split(':')); loc = datetime.now().astimezone()
    start = loc.replace(hour=hh, minute=mm, second=0, microsecond=0)
    return loc < start and loc.hour >= 6 and (start - loc).total_seconds() / 60

def cmd_go(a):
    early = too_early(a)
    if early: sys.exit(f'too early: not before {a.not_before} local ({early:.0f} min to go); refusing to slew')
    t = lookup(a.name); now = datetime.now(timezone.utc)
    if not (parse(t['window_start']) <= now <= parse(t['window_end'])):
        sys.exit(f'{t["key"]} is outside its observable window ({t["window_start"]} to {t["window_end"]}); refusing to slew')
    before = ctl('mount')
    print('pointing before:', json.dumps(before.get('mount', {}))[:300])
    print(f'going to {t["name"]}: RA {t["ra_hours"]:.4f} h Dec {t["dec_deg"]:+.3f}')
    ctl('monitor', '--lat', LAT, '--lon', LON, '--auto-stack', 'on')
    ctl('target', t['key'], '--ra-hours', t['ra_hours'], '--dec-deg', t['dec_deg'])
    ctl('goto', t['ra_hours'], t['dec_deg'])
    wait_idle()
    ctl('set', '--exposure', a.exposure, '--iso', a.iso)     # the telescope reverts these after a slew
    print(json.dumps({'ok': True, 'target': t['key']}))

def cmd_park(a):
    # parking is allowed at any time
    print(json.dumps(ctl('stack', check=False)))
    ctl('goto', *POLARIS); print(json.dumps({'ok': True, 'parked_at': 'Polaris'}))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter); sub = ap.add_subparsers(dest='c', required=True)
    sub.add_parser('now'); sub.add_parser('park')
    g = sub.add_parser('go'); g.add_argument('name'); g.add_argument('--exposure', type=float, default=10); g.add_argument('--iso', type=int, default=800); g.add_argument('--not-before', default='20:00', help='local HH:MM; no slewing earlier (the user puts the telescope out at 20:00)')
    a = ap.parse_args(); {'now': cmd_now, 'go': cmd_go, 'park': cmd_park}[a.c](a)

if __name__ == '__main__': main()
